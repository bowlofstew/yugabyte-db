// Copyright (c) YugaByte, Inc.

#include <memory>
#include <string>
#include <vector>

#include "yb/gutil/strings/substitute.h"
#include "yb/integration-tests/yb_table_test_base.h"
#include "yb/cqlserver/cql_server.h"
#include "yb/gutil/strings/join.h"
#include "yb/util/cast.h"
#include "yb/util/test_util.h"

namespace yb {
namespace cqlserver {

using std::string;
using std::unique_ptr;
using std::vector;
using strings::Substitute;
using yb::integration_tests::YBTableTestBase;

class TestCQLService : public YBTableTestBase {
 public:
  void SetUp() override;
  void TearDown() override;

 protected:
  void SendRequestAndExpectTimeout(const string& cmd);

  void SendRequestAndExpectResponse(const string& cmd, const string& resp);

  int server_port() { return cql_server_port_; }
 private:
  Status SendRequestAndGetResponse(
      const string& cmd, int expected_resp_length, int timeout_in_millis = 1000);

  Socket client_sock_;
  unique_ptr<boost::asio::io_service> io_;
  unique_ptr<CQLServer> server_;
  int cql_server_port_ = 0;
  unique_ptr<FileLock> cql_port_lock_;
  unique_ptr<FileLock> cql_webserver_lock_;
  static constexpr size_t kBufLen = 1024;
  uint8_t resp_[kBufLen];
};

void TestCQLService::SetUp() {
  YBTableTestBase::SetUp();

  CQLServerOptions opts;
  cql_server_port_ = GetFreePort(&cql_port_lock_);
  opts.rpc_opts.rpc_bind_addresses = strings::Substitute("0.0.0.0:$0", cql_server_port_);
  // No need to save the webserver port, as we don't plan on using it. Just use a unique free port.
  opts.webserver_opts.port = GetFreePort(&cql_webserver_lock_);
  string fs_root = GetTestPath("CQLServerTest-fsroot");
  opts.fs_opts.wal_paths = {fs_root};
  opts.fs_opts.data_paths = {fs_root};

  auto master_rpc_addrs = master_rpc_addresses_as_strings();
  opts.master_addresses_flag = JoinStrings(master_rpc_addrs, ",");

  io_.reset(new boost::asio::io_service());
  server_.reset(new CQLServer(opts, io_.get(), nullptr));
  LOG(INFO) << "Starting CQL server...";
  CHECK_OK(server_->Start());
  LOG(INFO) << "CQL server successfully started.";

  Sockaddr remote;
  CHECK_OK(remote.ParseString("0.0.0.0", server_port()));
  CHECK_OK(client_sock_.Init(0));
  CHECK_OK(client_sock_.SetNoDelay(false));
  LOG(INFO) << "Connecting to CQL server " << remote.ToString();
  CHECK_OK(client_sock_.Connect(remote));
}

void TestCQLService::TearDown() {
  EXPECT_OK(client_sock_.Close());
  YBTableTestBase::TearDown();
}

Status TestCQLService::SendRequestAndGetResponse(
    const string& cmd, int expected_resp_length, int timeout_in_millis) {
  // Send the request.
  int32_t bytes_written = 0;
  EXPECT_OK(client_sock_.Write(util::to_uchar_ptr(cmd.c_str()), cmd.length(), &bytes_written));

  EXPECT_EQ(cmd.length(), bytes_written);

  // Receive the response.
  MonoTime deadline = MonoTime::Now(MonoTime::FINE);
  deadline.AddDelta(MonoDelta::FromMilliseconds(timeout_in_millis));
  size_t bytes_read = 0;
  RETURN_NOT_OK(client_sock_.BlockingRecv(resp_, expected_resp_length, &bytes_read, deadline));
  if (expected_resp_length != bytes_read) {
    return STATUS(
        IOError, Substitute("Received $1 bytes instead of $2", bytes_read, expected_resp_length));
  }
  return Status::OK();
}

void TestCQLService::SendRequestAndExpectTimeout(const string& cmd) {
  // Don't expect to receive even 1 byte.
  ASSERT_TRUE(SendRequestAndGetResponse(cmd, 1).IsTimedOut());
}

void TestCQLService::SendRequestAndExpectResponse(const string& cmd, const string& resp) {
  CHECK_OK(SendRequestAndGetResponse(cmd, resp.length()));

  // Verify that the response is as expected.
  CHECK_EQ(resp, string(reinterpret_cast<char*>(resp_), resp.length()));
}

// The following test cases test the CQL protocol marshalling/unmarshalling with hand-coded
// request messages and expected responses. They are good as basic and error-handling tests.
// These are expected to be few.
//
// TODO(Robert) - add more tests using Cassandra C++ driver when the CQL server can respond
// to queries.
TEST_F(TestCQLService, StartupRequest) {
  LOG(INFO) << "Test CQL STARTUP request";
  // Send STARTUP request using version V3
  SendRequestAndExpectResponse(
      BINARY_STRING("\x03\x00\x00\x00\x01" "\x00\x00\x00\x16"
                    "\x00\x01" "\x00\x0b" "CQL_VERSION"
                               "\x00\x05" "3.0.0"),
      BINARY_STRING("\x83\x00\x00\x00\x02" "\x00\x00\x00\x00"));

  // Send STARTUP request using version V4
  SendRequestAndExpectResponse(
      BINARY_STRING("\x04\x00\x00\x00\x01" "\x00\x00\x00\x16"
                    "\x00\x01" "\x00\x0b" "CQL_VERSION"
                               "\x00\x05" "3.0.0"),
      BINARY_STRING("\x84\x00\x00\x00\x02" "\x00\x00\x00\x00"));

  // Send STARTUP request using version V5
  SendRequestAndExpectResponse(
      BINARY_STRING("\x05\x00\x00\x00\x01" "\x00\x00\x00\x16"
                    "\x00\x01" "\x00\x0b" "CQL_VERSION"
                               "\x00\x05" "3.0.0"),
      BINARY_STRING("\x84\x00\x00\x00\x00" "\x00\x00\x00\x4f"
                    "\x00\x00\x00\x0a" "\x00\x49"
                    "Protocol version 5 not supported. Supported versions are between 3 and 4."));
}

TEST_F(TestCQLService, OptionsRequest) {
  LOG(INFO) << "Test CQL OPTIONS request";
  // Send OPTIONS request using version V4
  SendRequestAndExpectResponse(
      BINARY_STRING("\x04\x00\x00\x00\x05" "\x00\x00\x00\x00"),
      BINARY_STRING("\x84\x00\x00\x00\x06" "\x00\x00\x00\x2e"
                    "\x00\x02" "\x00\x0b" "CQL_VERSION"
                               "\x00\x02" "\x00\x05" "3.0.0" "\x00\x05" "3.4.2"
                               "\x00\x0b" "COMPRESSION"
                               "\x00\x00"));
}

TEST_F(TestCQLService, InvalidRequest) {
  LOG(INFO) << "Test invalid CQL request";
  // Send response (0x84) as request
  SendRequestAndExpectResponse(
      BINARY_STRING("\x84\x00\x00\x00\x00" "\x00\x00\x00\x22"
                    "\x00\x00\x00\x0a" "\x00\x1c" "Unsupported protocol version"),
      BINARY_STRING("\x84\x00\x00\x00\x00" "\x00\x00\x00\x13"
                    "\x00\x00\x00\x0a" "\x00\x0d" "Not a request"));

  // Send ERROR as request
  SendRequestAndExpectResponse(
      BINARY_STRING("\x04\x00\x00\x00\x00" "\x00\x00\x00\x22"
                    "\x00\x00\x00\x0a" "\x00\x1c" "Unsupported protocol version"),
      BINARY_STRING("\x84\x00\x00\x00\x00" "\x00\x00\x00\x1a"
                    "\x00\x00\x00\x0a" "\x00\x14" "Not a request opcode"));

  // Send an unknown opcode
  SendRequestAndExpectResponse(
      BINARY_STRING("\x04\x00\x00\x00\xff" "\x00\x00\x00\x22"
                    "\x00\x00\x00\x0a" "\x00\x1c" "Unsupported protocol version"),
      BINARY_STRING("\x84\x00\x00\x00\x00" "\x00\x00\x00\x14"
                    "\x00\x00\x00\x0a" "\x00\x0e" "Unknown opcode"));

  // Send truncated request
  SendRequestAndExpectResponse(
      BINARY_STRING("\x04\x00\x00\x00\x01" "\x00\x00\x00\x0f"
                    "\x00\x01" "\x00\x0b" "CQL_VERSION"),
      BINARY_STRING("\x84\x00\x00\x00\x00" "\x00\x00\x00\x1b"
                    "\x00\x00\x00\x0a" "\x00\x15" "Truncated CQL message"));

  // Send truncated string in request
  SendRequestAndExpectResponse(
      BINARY_STRING("\x04\x00\x00\x00\x01" "\x00\x00\x00\x16"
                    "\x00\x01" "\x00\x0b" "CQL_VERSION"
                               "\x00\x15" "3.0.0"),
      BINARY_STRING("\x84\x00\x00\x00\x00" "\x00\x00\x00\x1b"
                    "\x00\x00\x00\x0a" "\x00\x15" "Truncated CQL message"));

  // Send request with extra trailing bytes
  SendRequestAndExpectResponse(
      BINARY_STRING("\x04\x00\x00\x00\x01" "\x00\x00\x00\x18"
                    "\x00\x01" "\x00\x0b" "CQL_VERSION"
                               "\x00\x05" "3.0.0"
                               "\x00\0x00"),
      BINARY_STRING("\x84\x00\x00\x00\x00" "\x00\x00\x00\x1d"
                    "\x00\x00\x00\x0a" "\x00\x17" "Request length too long"));
}

}  // namespace cqlserver
}  // namespace yb
