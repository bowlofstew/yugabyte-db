//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// Entry point for the semantic analytical process.
//--------------------------------------------------------------------------------------------------

#ifndef YB_SQL_SEM_ANALYZER_H_
#define YB_SQL_SEM_ANALYZER_H_

#include "yb/sql/ptree/sem_context.h"

namespace yb {
namespace sql {

//--------------------------------------------------------------------------------------------------

class Analyzer {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef std::unique_ptr<Analyzer> UniPtr;
  typedef std::unique_ptr<const Analyzer> UniPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor & destructor.
  Analyzer();
  virtual ~Analyzer();

  // Run semantics analysis on the given parse tree and decorate it with semantics information such
  // as datatype or object-type of a database object.
  ErrorCode Analyze(const std::string& sql_stmt,
                    ParseTree::UniPtr ptree,
                    SqlEnv *sql_env,
                    int retry_count);

  // Returns decorated parse tree from the semantic analysis.
  ParseTree::UniPtr Done();

 private:
  SemContext::UniPtr sem_context_;
};

}  // namespace sql
}  // namespace yb

#endif  // YB_SQL_SEM_ANALYZER_H_