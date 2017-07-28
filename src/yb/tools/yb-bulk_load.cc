// Copyright (c) YugaByte, Inc.

#include <sched.h>
#include <iostream>
#include <thread>
#include <boost/algorithm/string.hpp>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "rocksdb/db.h"
#include "yb/client/client.h"
#include "yb/common/entity_ids.h"
#include "yb/common/hybrid_time.h"
#include "yb/common/schema.h"
#include "yb/common/wire_protocol.h"
#include "yb/common/yql_protocol.pb.h"
#include "yb/docdb/docdb.h"
#include "yb/docdb/docdb_test_util.h"
#include "yb/docdb/doc_operation.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc_controller.h"
#include "yb/tools/bulk_load_docdb_util.h"
#include "yb/tools/bulk_load_utils.h"
#include "yb/tools/yb-generate_partitions.h"
#include "yb/tserver/tserver_service.proxy.h"
#include "yb/util/status.h"
#include "yb/util/stopwatch.h"
#include "yb/util/size_literals.h"
#include "yb/util/threadpool.h"
#include "yb/util/flags.h"
#include "yb/util/logging.h"
#include "yb/util/path_util.h"
#include "yb/util/subprocess.h"

using std::pair;
using std::stoi;
using std::stol;
using std::stold;
using std::string;
using std::shared_ptr;
using std::unique_ptr;
using std::vector;
using yb::client::YBClient;
using yb::client::YBClientBuilder;
using yb::client::YBTable;
using yb::client::YBTableName;
using yb::operator"" _GB;

DEFINE_string(master_addresses, "", "Comma-separated list of YB Master server addresses");
DEFINE_string(table_name, "", "Name of the table to generate partitions for");
DEFINE_string(namespace_name, "", "Namespace of the table");
DEFINE_string(base_dir, "", "Base directory where we will store all the SSTable files");
DEFINE_int64(memtable_size_bytes, 1_GB, "Amount of bytes to use for the rocksdb memtable");
DEFINE_int32(row_batch_size, 1000, "The number of rows to batch together in each rocksdb write");
DEFINE_string(bulk_load_helper_script, "./bulk_load_helper.sh", "Relative path for bulk load helper"
              " script");
DEFINE_string(bulk_load_cleanup_script, "./bulk_load_cleanup.sh", "Relative path for bulk load "
              "cleanup script");
DEFINE_string(ssh_key_file, "", "SSH key to push SSTable files to production cluster");
DEFINE_bool(export_files, false, "Whether or not the files should be exported to a production "
            "cluster.");
DEFINE_int32(bulk_load_num_threads, 16, "Number of threads to use for bulk load");
DEFINE_int32(bulk_load_threadpool_queue_size, 10000,
             "Maximum number of entries to queue in the threadpool");
DEFINE_int32(bulk_load_num_memtables, 3, "Number of memtables to use for rocksdb");
DEFINE_int32(bulk_load_max_background_flushes, 2, "Number of flushes to perform in the background");

namespace yb {
namespace tools {

namespace {

class BulkLoadTask : public Runnable {
 public:
  BulkLoadTask(vector<pair<TabletId, string>> rows, BulkLoadDocDBUtil *db_fixture,
               const YBTable *table, YBPartitionGenerator *partition_generator);
  void Run();
 private:
  CHECKED_STATUS PopulateColumnValue(const string &column,
                                     const DataType data_type,
                                     YQLColumnValuePB *column_value);
  CHECKED_STATUS InsertRow(const string &row,
                           const Schema &schema,
                           BulkLoadDocDBUtil *const db_fixture,
                           docdb::DocWriteBatch *const doc_write_batch,
                           YBPartitionGenerator *const partition_generator);
  vector<pair<TabletId, string>> rows_;
  BulkLoadDocDBUtil *const db_fixture_;
  const YBTable *const table_;
  YBPartitionGenerator *const partition_generator_;
};

class BulkLoad {
 public:
  CHECKED_STATUS RunBulkLoad();

 private:
  CHECKED_STATUS InitYBBulkLoad();
  CHECKED_STATUS InitDBUtil(const TabletId &tablet_id);
  CHECKED_STATUS FinishTabletProcessing(const TabletId &tablet_id,
                                        vector<pair<TabletId, string>> rows);
  CHECKED_STATUS RetryableSubmit(vector<pair<TabletId, string>> rows);

  shared_ptr<YBClient> client_;
  shared_ptr<YBTable> table_;
  unique_ptr<YBPartitionGenerator> partition_generator_;
  gscoped_ptr<ThreadPool> thread_pool_;
  unique_ptr<BulkLoadDocDBUtil> db_fixture_;
};

BulkLoadTask::BulkLoadTask(vector<pair<TabletId, string>> rows,
                           BulkLoadDocDBUtil *db_fixture, const YBTable *table,
                           YBPartitionGenerator *partition_generator)
    : rows_(std::move(rows)),
      db_fixture_(db_fixture),
      table_(table),
      partition_generator_(partition_generator) {
}

void BulkLoadTask::Run() {
  auto doc_write_batch = std::make_unique<docdb::DocWriteBatch>(db_fixture_->rocksdb());

  for (const auto &entry : rows_) {
    const string &row = entry.second;

    // Populate the row.
    CHECK_OK(InsertRow(row, table_->InternalSchema(), db_fixture_, doc_write_batch.get(),
                       partition_generator_));
  }

  // Flush the batch.
  CHECK_OK(db_fixture_->WriteToRocksDB(
      *doc_write_batch, HybridTime::FromMicros(kYugaByteMicrosecondEpoch),
      /* decode_dockey */ false, /* increment_write_id */ false));
}

Status BulkLoadTask::PopulateColumnValue(const string &column,
                                         const DataType data_type,
                                         YQLColumnValuePB *column_value) {
  auto yql_valuepb = column_value->mutable_expr()->mutable_value();
  int32_t int_val;
  int64_t long_val;
  double double_val;
  switch (data_type) {
    case DataType::INT8:
      RETURN_NOT_OK(CheckedStoi(column, &int_val));
      yql_valuepb->set_int8_value(int_val);
      break;
    case DataType::INT16:
      RETURN_NOT_OK(CheckedStoi(column, &int_val));
      yql_valuepb->set_int16_value(int_val);
      break;
    case DataType::INT32:
      RETURN_NOT_OK(CheckedStoi(column, &int_val));
      yql_valuepb->set_int32_value(stoi(column));
      break;
    case DataType::INT64:
      RETURN_NOT_OK(CheckedStol(column, &long_val));
      yql_valuepb->set_int64_value(stol(column));
      break;
    case DataType::FLOAT:
      RETURN_NOT_OK(CheckedStold(column, &double_val));
      yql_valuepb->set_float_value(double_val);
      break;
    case DataType::DOUBLE:
      RETURN_NOT_OK(CheckedStold(column, &double_val));
      yql_valuepb->set_double_value(double_val);
      break;
    case DataType::STRING:
      yql_valuepb->set_string_value(column);
      break;
    case DataType::TIMESTAMP: {
      Timestamp ts;
      RETURN_NOT_OK(TimestampFromString(column, &ts));
      yql_valuepb->set_timestamp_value(ts.ToInt64());
      break;
    }
    default:
      FATAL_INVALID_ENUM_VALUE(DataType, data_type);
  }
  return Status::OK();
}

Status BulkLoadTask::InsertRow(const string &row,
                               const Schema &schema,
                               BulkLoadDocDBUtil *const db_fixture,
                               docdb::DocWriteBatch *const doc_write_batch,
                               YBPartitionGenerator *const partition_generator) {
  // Get individual columns.
  CsvTokenizer tokenizer = Tokenize(row);
  size_t ncolumns = std::distance(tokenizer.begin(), tokenizer.end());
  if (ncolumns != schema.num_columns()) {
    return STATUS_SUBSTITUTE(IllegalState, "row '$0' has $1 columns, need exactly $2", row,
                             ncolumns, schema.num_columns());
  }

  YQLResponsePB resp;
  YQLWriteRequestPB req;
  req.set_type(YQLWriteRequestPB_YQLStmtType_YQL_STMT_INSERT);
  req.set_client(YQL_CLIENT_CQL);

  auto it = tokenizer.begin();
  // Process the hash keys first.
  for (int i = 0; i < schema.num_key_columns(); i++, it++) {
    if (IsNull(*it)) {
      return STATUS_SUBSTITUTE(IllegalState, "Primary key cannot be null: $0", *it);
    }

    YQLColumnValuePB *column_value = nullptr;
    if (schema.is_hash_key_column(i)) {
      column_value = req.add_hashed_column_values();
    } else {
      column_value = req.add_range_column_values();
    }

    column_value->set_column_id(kFirstColumnId + i);
    RETURN_NOT_OK(PopulateColumnValue(*it, schema.column(i).type_info()->type(),
                                          column_value));
  }

  // Finally process the regular columns.
  for (int i = schema.num_key_columns(); i < schema.num_columns(); i++, it++) {
    YQLColumnValuePB *column_value = req.add_column_values();
    column_value->set_column_id(kFirstColumnId + i);
    if (IsNull(*it)) {
      // Use empty value for null.
      column_value->mutable_expr()->mutable_value();
    } else {
      RETURN_NOT_OK(PopulateColumnValue(*it, schema.column(i).type_info()->type(),
                                            column_value));
    }
  }

  // Add the hash code to the operation.
  string tablet_id;
  string partition_key;
  RETURN_NOT_OK(partition_generator->LookupTabletIdWithTokenizer(tokenizer, &tablet_id,
                                                                     &partition_key));
  req.set_hash_code(PartitionSchema::DecodeMultiColumnHashValue(partition_key));

  // Finally apply the operation to the the doc_write_batch.
  docdb::YQLWriteOperation op(req, schema, &resp);
  RETURN_NOT_OK(op.Apply(doc_write_batch, db_fixture->rocksdb(),
                             HybridTime::FromMicros(kYugaByteMicrosecondEpoch)));
  return Status::OK();
}


Status BulkLoad::RetryableSubmit(vector<pair<TabletId, string>> rows) {
  auto runnable = std::make_shared<BulkLoadTask>(
      std::move(rows), db_fixture_.get(), table_.get(), partition_generator_.get());

  Status s;
  do {
    s = thread_pool_->Submit(runnable);

    if (!s.IsServiceUnavailable()) {
      return s;
    }

    LOG (ERROR) << "Failed submitting task, sleeping for a while: " << s.ToString();

    // If service is unavailable, the queue might be full. Sleep and try again.
    SleepFor(MonoDelta::FromSeconds(10));
  } while (!s.ok());

  return Status::OK();
}

Status BulkLoad::FinishTabletProcessing(const TabletId &tablet_id,
                                        vector<pair<TabletId, string>> rows) {
  if (!db_fixture_) {
    // Skip processing since db_fixture wasn't initialized indicating empty input.
    return Status::OK();
  }

  // Submit all the work.
  RETURN_NOT_OK(RetryableSubmit(std::move(rows)));

  // Wait for all tasks for the tablet to complete.
  thread_pool_->Wait();

  // Now flush the DB.
  RETURN_NOT_OK(db_fixture_->FlushRocksDB());

  if (!FLAGS_export_files) {
    return Status::OK();
  }

  // Find replicas for the tablet.
  master::TabletLocationsPB tablet_locations;
  RETURN_NOT_OK(client_->GetTabletLocation(tablet_id, &tablet_locations));
  string csv_replicas;
  std::map<string, int32_t> host_to_rpcport;
  for (const master::TabletLocationsPB_ReplicaPB &replica : tablet_locations.replicas()) {
    if (!csv_replicas.empty()) {
      csv_replicas += ",";
    }
    const string &host = replica.ts_info().rpc_addresses(0).host();
    csv_replicas += host;
    host_to_rpcport[host] = replica.ts_info().rpc_addresses(0).port();
  }

  // Invoke the bulk_load_helper script.
  vector<string> argv = {FLAGS_bulk_load_helper_script, "-t", tablet_id, "-r", csv_replicas, "-i",
      FLAGS_ssh_key_file, "-d", db_fixture_->rocksdb_dir()};
  string bulk_load_helper_stdout;
  RETURN_NOT_OK(Subprocess::Call(argv, &bulk_load_helper_stdout));

  // Trim the output.
  boost::trim(bulk_load_helper_stdout);
  LOG(INFO) << "Helper script stdout: " << bulk_load_helper_stdout;

  // Finalize the import.
  rpc::MessengerBuilder bld("Client");
  std::shared_ptr<rpc::Messenger> client_messenger;
  RETURN_NOT_OK(bld.Build(&client_messenger));
  vector<string> lines;
  boost::split(lines, bulk_load_helper_stdout, boost::is_any_of("\n"));
  for (const string &line : lines) {
    vector<string> tokens;
    boost::split(tokens, line, boost::is_any_of(","));
    if (tokens.size() != 2) {
      return STATUS_SUBSTITUTE(InvalidArgument, "Invalid line $0", line);
    }
    const string &replica_host = tokens[0];
    const string &directory = tokens[1];
    Endpoint endpoint(IpAddress::from_string(replica_host), host_to_rpcport[replica_host]);

    tserver::TabletServerServiceProxy proxy(client_messenger, endpoint);
    tserver::ImportDataRequestPB req;
    req.set_tablet_id(tablet_id);
    req.set_source_dir(directory);

    tserver::ImportDataResponsePB resp;
    rpc::RpcController controller;
    LOG(INFO) << "Importing " << directory << " on " << replica_host << " for tablet_id: "
              << tablet_id;
    RETURN_NOT_OK(proxy.ImportData(req, &resp, &controller));
    if (resp.has_error()) {
      RETURN_NOT_OK(StatusFromPB(resp.error().status()));
    }

    // Now cleanup the files from the production tserver.
    vector<string> cleanup_script = {FLAGS_bulk_load_cleanup_script, "-d", directory, "-t",
        replica_host, "-i", FLAGS_ssh_key_file};
    RETURN_NOT_OK(Subprocess::Call(cleanup_script));
  }

  // Delete the data once the import is done.
  return yb::Env::Default()->DeleteRecursively(db_fixture_->rocksdb_dir());
}


CHECKED_STATUS BulkLoad::InitDBUtil(const TabletId &tablet_id) {
  db_fixture_.reset(new BulkLoadDocDBUtil(tablet_id, FLAGS_base_dir,
                                          FLAGS_memtable_size_bytes,
                                          FLAGS_bulk_load_num_memtables,
                                          FLAGS_bulk_load_max_background_flushes));
  RETURN_NOT_OK(db_fixture_->InitRocksDBOptions());
  RETURN_NOT_OK(db_fixture_->DisableCompactions()); // This opens rocksdb.
  return Status::OK();
}

Status BulkLoad::InitYBBulkLoad() {
  // Convert table_name to lowercase since we store table names in lowercase.
  string table_name_lower = boost::to_lower_copy(FLAGS_table_name);
  YBTableName table_name(FLAGS_namespace_name, table_name_lower);

  YBClientBuilder builder;
  builder.add_master_server_addr(FLAGS_master_addresses);

  RETURN_NOT_OK(builder.Build(&client_));
  RETURN_NOT_OK(client_->OpenTable(table_name, &table_));
  partition_generator_.reset(new YBPartitionGenerator(table_name, {FLAGS_master_addresses}));
  RETURN_NOT_OK(partition_generator_->Init());

  db_fixture_ = nullptr;
  CHECK_OK(
      ThreadPoolBuilder("bulk_load_tasks")
          .set_min_threads(FLAGS_bulk_load_num_threads)
          .set_max_threads(FLAGS_bulk_load_num_threads)
          .set_max_queue_size(FLAGS_bulk_load_threadpool_queue_size)
          .set_idle_timeout(MonoDelta::FromMilliseconds(5000))
          .Build(&thread_pool_));
  return Status::OK();
}


Status BulkLoad::RunBulkLoad() {

  RETURN_NOT_OK(InitYBBulkLoad());

  TabletId current_tablet_id;

  vector<pair<TabletId, string>> rows;
  for (string line; std::getline(std::cin, line);) {
    // Trim the line.
    boost::algorithm::trim(line);

    // Get the key and value.
    std::size_t index = line.find("\t");
    if (index == std::string::npos) {
      return STATUS_SUBSTITUTE(IllegalState, "Invalid line: $0", line);
    }
    const TabletId tablet_id = line.substr(0, index);
    const string row = line.substr(index + 1, line.size() - (index + 1));

    // Reinitialize rocksdb if needed.
    if (current_tablet_id.empty() || current_tablet_id != tablet_id) {
      // Flush all of the data before opening a new rocksdb.
      RETURN_NOT_OK(FinishTabletProcessing(current_tablet_id, std::move(rows)));
      RETURN_NOT_OK(InitDBUtil(tablet_id));
    }
    current_tablet_id = tablet_id;
    rows.emplace_back(std::move(tablet_id), std::move(row));

    // Flush the batch if necessary.
    if (rows.size() >= FLAGS_row_batch_size) {
      RETURN_NOT_OK(RetryableSubmit(std::move(rows)));
    }
  }

  // Process last tablet.
  RETURN_NOT_OK(FinishTabletProcessing(current_tablet_id, std::move(rows)));
  return Status::OK();
}

} // anonymous namespace

} // namespace tools
} // namespace yb

int main(int argc, char** argv) {
  yb::ParseCommandLineFlags(&argc, &argv, true);
  yb::InitGoogleLoggingSafe(argv[0]);
  if (FLAGS_master_addresses.empty() || FLAGS_table_name.empty() || FLAGS_namespace_name.empty()
      || FLAGS_base_dir.empty()) {
    LOG(FATAL) << "Need to specify --master_addresses, --table_name, --namespace_name, "
        "--base_dir";
  }

  if (FLAGS_export_files && FLAGS_ssh_key_file.empty()) {
    LOG(FATAL) << "Need to specify --ssh_key_file with --export_files";
  }

  // Verify the bulk load path exists.
  if (!yb::Env::Default()->FileExists(FLAGS_base_dir)) {
    LOG(FATAL) << "Bulk load directory doesn't exist: " << FLAGS_base_dir;
  }

  yb::tools::BulkLoad bulk_load;
  yb::Status s = bulk_load.RunBulkLoad();
  if (!s.ok()) {
    LOG(FATAL) << "Error running bulk load: " << s.ToString();
  }
  return 0;
}