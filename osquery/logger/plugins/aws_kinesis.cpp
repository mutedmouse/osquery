/*
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <algorithm>

#include <boost/algorithm/string/join.hpp>

#include <aws/core/utils/Outcome.h>
#include <aws/kinesis/model/DescribeStreamRequest.h>
#include <aws/kinesis/model/DescribeStreamResult.h>
#include <aws/kinesis/model/PutRecordsRequest.h>
#include <aws/kinesis/model/PutRecordsRequestEntry.h>
#include <aws/kinesis/model/PutRecordsResult.h>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <osquery/flags.h>
#include <osquery/registry.h>
#include <osquery/system.h>

#include "osquery/logger/plugins/aws_kinesis.h"
#include "osquery/logger/plugins/aws_util.h"

namespace osquery {

REGISTER(KinesisLoggerPlugin, "logger", "aws_kinesis");

FLAG(uint64,
     aws_kinesis_period,
     10,
     "Seconds between flushing logs to Kinesis (default 10)");
FLAG(string, aws_kinesis_stream, "", "Name of Kinesis stream for logging")
FLAG(bool,
     aws_kinesis_random_partition_key,
     false,
     "Enable random kinesis partition keys");

// This is the max per AWS docs
const size_t KinesisLogForwarder::kKinesisMaxRecords = 500;
// Max size of log + partition key is 1MB. Max size of partition key is 256B.
const size_t KinesisLogForwarder::kKinesisMaxLogBytes = 1000000 - 256;

Status KinesisLoggerPlugin::setUp() {
  initAwsSdk();
  forwarder_ = std::make_shared<KinesisLogForwarder>();
  Status s = forwarder_->setUp();
  if (!s.ok()) {
    LOG(ERROR) << "Error initializing Kinesis logger: " << s.getMessage();
    return s;
  }
  Dispatcher::addService(forwarder_);
  return Status(0, "OK");
}

Status KinesisLoggerPlugin::logString(const std::string& s) {
  return forwarder_->logString(s);
}

Status KinesisLogForwarder::send(std::vector<std::string>& log_data,
                                 const std::string& log_type) {
  std::vector<Aws::Kinesis::Model::PutRecordsRequestEntry> entries;
  if (FLAGS_aws_kinesis_random_partition_key) {
    boost::uuids::uuid uuid = boost::uuids::random_generator()();
    partition_key_ = boost::uuids::to_string(uuid);
  }
  for (const std::string& log : log_data) {
    if (log.size() > kKinesisMaxLogBytes) {
      LOG(ERROR) << "Kinesis log too big, discarding!";
    }
    Aws::Kinesis::Model::PutRecordsRequestEntry entry;
    entry.WithPartitionKey(partition_key_)
        .WithData(
            Aws::Utils::ByteBuffer((unsigned char*)log.c_str(), log.length()));
    entries.push_back(std::move(entry));
  }

  Aws::Kinesis::Model::PutRecordsRequest request;
  request.WithStreamName(FLAGS_aws_kinesis_stream)
      .WithRecords(std::move(entries));

  Aws::Kinesis::Model::PutRecordsOutcome outcome = client_->PutRecords(request);
  Aws::Kinesis::Model::PutRecordsResult result = outcome.GetResult();
  if (result.GetFailedRecordCount() != 0) {
    for (const auto& record : result.GetRecords()) {
      if (!record.GetErrorMessage().empty()) {
        LOG(ERROR) << "Kinesis write for " << result.GetFailedRecordCount()
                   << " of " << result.GetRecords().size()
                   << " records failed with error " << record.GetErrorMessage();
        return Status(1, record.GetErrorMessage());
      }
    }
  }

  VLOG(1) << "Successfully sent " << result.GetRecords().size()
          << " logs to Kinesis.";
  return Status(0);
}

Status KinesisLogForwarder::setUp() {
  Status s = BufferedLogForwarder::setUp();
  if (!s.ok()) {
    return s;
  }

  // Set up client
  s = makeAWSClient<Aws::Kinesis::KinesisClient>(client_);
  if (!s.ok()) {
    return s;
  }

  partition_key_ = getHostIdentifier();

  if (FLAGS_aws_kinesis_stream.empty()) {
    return Status(1, "Stream name must be specified with --aws_kinesis_stream");
  }

  // Make sure we can connect to designated stream
  Aws::Kinesis::Model::DescribeStreamRequest r;
  r.SetStreamName(FLAGS_aws_kinesis_stream);
  auto outcome = client_->DescribeStream(r);
  if (!outcome.IsSuccess()) {
    return Status(1,
                  "Could not find Kinesis stream: " + FLAGS_aws_kinesis_stream);
  }
  VLOG(1) << "Kinesis logging initialized with stream: "
          << FLAGS_aws_kinesis_stream;
  return Status(0);
}
}
