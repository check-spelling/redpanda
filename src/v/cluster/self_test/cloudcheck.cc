/*
 * Copyright 2023 Redpanda Data, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#include "cluster/self_test/cloudcheck.h"

#include "base/vlog.h"
#include "cluster/logger.h"
#include "config/configuration.h"
#include "random/generators.h"
#include "utils/uuid.h"

#include <algorithm>

namespace cluster::self_test {

cloudcheck::cloudcheck(ss::sharded<cloud_storage::remote>& cloud_storage_api)
  : _rtc(_as)
  , _cloud_storage_api(cloud_storage_api) {}

ss::future<> cloudcheck::start() { return ss::make_ready_future<>(); }

ss::future<> cloudcheck::stop() {
    _as.request_abort();
    return _gate.close();
}

void cloudcheck::cancel() { _cancelled = true; }

ss::future<std::vector<self_test_result>>
cloudcheck::run(cloudcheck_opts opts) {
    _cancelled = false;
    _opts = std::move(opts);

    if (_gate.is_closed()) {
        vlog(clusterlog.debug, "cloudcheck - gate already closed");
        auto result = self_test_result{
          .name = _opts.name, .warning = "cloudcheck - gate already closed"};
        co_return std::vector<self_test_result>{result};
    }
    auto g = _gate.hold();

    vlog(
      clusterlog.info,
      "Starting redpanda self-test cloud benchmark, with options: {}",
      opts);

    const auto& cfg = config::shard_local_cfg();
    if (!cfg.cloud_storage_enabled()) {
        vlog(
          clusterlog.warn,
          "Cloud storage is not enabled, exiting cloud storage self-test.");
        auto result = self_test_result{
          .name = _opts.name, .warning = "Cloud storage is not enabled."};
        co_return std::vector<self_test_result>{result};
    }

    if (!_cloud_storage_api.local_is_initialized()) {
        vlog(
          clusterlog.warn,
          "_cloud_storage_api is not initialized, exiting cloud storage "
          "self-test.");
        auto result = self_test_result{
          .name = _opts.name,
          .warning = "cloud_storage_api is not initialized."};
        co_return std::vector<self_test_result>{result};
    }

    _remote_read_enabled = cfg.cloud_storage_enable_remote_read();
    _remote_write_enabled = cfg.cloud_storage_enable_remote_write();

    co_return co_await ss::with_scheduling_group(
      _opts.sg, [this]() mutable { return run_benchmarks(); });
}

ss::future<std::vector<self_test_result>> cloudcheck::run_benchmarks() {
    std::vector<self_test_result> results;

    const auto bucket = cloud_storage_clients::bucket_name{
      cloud_storage::configuration::get_bucket_config()().value()};

    const auto self_test_prefix = cloud_storage_clients::object_key{
      "self-test/"};

    const auto uuid = cloud_storage_clients::object_key{
      ss::sstring{uuid_t::create()}};
    const auto self_test_key = cloud_storage_clients::object_key{
      self_test_prefix / uuid};

    const std::optional<iobuf> payload = (_remote_write_enabled)
                                           ? make_random_payload()
                                           : std::optional<iobuf>{};

    // Test Upload
    auto upload_test_result = co_await verify_upload(
      bucket, self_test_key, payload);
    const bool is_uploaded
      = (!upload_test_result.warning && !upload_test_result.error);

    results.push_back(std::move(upload_test_result));

    // Test List
    auto list_test_result_pair = co_await verify_list(bucket, self_test_prefix);
    auto& [object_list, list_test_result] = list_test_result_pair;
    if (is_uploaded && object_list) {
        // Check that uploaded object exists in object_list contents.
        auto& object_list_contents = object_list.value().contents;
        auto payload_item_it = std::find_if(
          object_list_contents.begin(),
          object_list_contents.end(),
          [self_test_key](const auto& item) {
              return item.key == self_test_key();
          });

        if (payload_item_it == object_list_contents.end()) {
            list_test_result.error = "Uploaded key/payload could not be found "
                                     "in cloud storage item list.";
        }
    }

    results.push_back(std::move(list_test_result));

    // Test Download
    // If we have uploaded our payload, we will attempt to get the written
    // object. If we didn't, we will attempt to get the smallest object from the
    // object list, if at least one exists.
    auto get_min_object_key =
      [&object_list]() -> std::optional<cloud_storage_clients::object_key> {
        if (!object_list) {
            return std::nullopt;
        }

        auto& object_list_contents = object_list.value().contents;
        if (object_list_contents.empty()) {
            return std::nullopt;
        }

        // Get the smallest file from object_list.
        auto smallest_object = *std::min_element(
          object_list_contents.begin(),
          object_list_contents.end(),
          [](const auto& a, const auto& b) {
              return a.size_bytes < b.size_bytes;
          });
        return cloud_storage_clients::object_key{smallest_object.key};
    };

    const std::optional<cloud_storage_clients::object_key> download_key
      = (is_uploaded) ? self_test_key : get_min_object_key();
    auto download_test_result_pair = co_await verify_download(
      bucket, download_key);
    auto& [downloaded_object, download_test_result] = download_test_result_pair;
    if (is_uploaded && downloaded_object) {
        auto& downloaded_buf = downloaded_object.value();
        if (downloaded_buf != payload.value()) {
            download_test_result.error
              = "Downloaded object differs from uploaded payload.";
        }
    }

    results.push_back(std::move(download_test_result));

    // Test Delete
    auto delete_test_result = co_await verify_delete(bucket, self_test_key);
    results.push_back(std::move(delete_test_result));

    co_return results;
}

iobuf cloudcheck::make_random_payload(size_t size) const {
    iobuf payload;
    const auto random_data = random_generators::gen_alphanum_string(size);
    payload.append(random_data.data(), size);
    return payload;
}

cloud_storage::upload_request cloudcheck::make_upload_request(
  const cloud_storage_clients::bucket_name& bucket,
  const cloud_storage_clients::object_key& key,
  iobuf payload,
  retry_chain_node& rtc) {
    cloud_storage::transfer_details transfer_details{
      .bucket = bucket, .key = key, .parent_rtc = rtc};
    cloud_storage::upload_request upload_request(
      std::move(transfer_details),
      cloud_storage::upload_type::object,
      std::move(payload));
    return upload_request;
}

cloud_storage::download_request cloudcheck::make_download_request(
  const cloud_storage_clients::bucket_name& bucket,
  const cloud_storage_clients::object_key& key,
  iobuf& payload,
  retry_chain_node& rtc) {
    cloud_storage::transfer_details transfer_details{
      .bucket = bucket, .key = key, .parent_rtc = rtc};
    cloud_storage::download_request download_request(
      std::move(transfer_details),
      cloud_storage::download_type::object,
      std::ref(payload));
    return download_request;
}

ss::future<self_test_result> cloudcheck::verify_upload(
  cloud_storage_clients::bucket_name bucket,
  cloud_storage_clients::object_key key,
  const std::optional<iobuf>& payload) {
    auto result = self_test_result{
      .name = _opts.name, .info = "upload", .test_type = "cloud_storage"};

    if (_cancelled) {
        result.warning = "Run was manually cancelled.";
        co_return result;
    }

    if (!_remote_write_enabled) {
        result.error = "Remote write is not enabled for this cluster.";
        co_return result;
    }

    auto rtc = retry_chain_node(_opts.timeout, _opts.backoff, &_rtc);
    cloud_storage::upload_request upload_request = make_upload_request(
      bucket, key, payload.value().copy(), rtc);

    const auto start = ss::lowres_clock::now();
    try {
        const cloud_storage::upload_result upload_result
          = co_await _cloud_storage_api.local().upload_object(
            std::move(upload_request));

        switch (upload_result) {
        case cloud_storage::upload_result::success:
            break;
        case cloud_storage::upload_result::timedout:
            [[fallthrough]];
        case cloud_storage::upload_result::failed:
            [[fallthrough]];
        case cloud_storage::upload_result::cancelled:
            result.error = "Failed to upload to cloud storage.";
            break;
        }
    } catch (const std::exception& e) {
        result.error = e.what();
    }

    const auto end = ss::lowres_clock::now();
    result.duration = end - start;

    co_return result;
}

ss::future<std::pair<cloud_storage::remote::list_result, self_test_result>>
cloudcheck::verify_list(
  cloud_storage_clients::bucket_name bucket,
  cloud_storage_clients::object_key prefix,
  size_t max_keys) {
    auto result = self_test_result{
      .name = _opts.name, .info = "list", .test_type = "cloud_storage"};

    if (_cancelled) {
        result.warning = "Run was manually cancelled.";
        co_return std::make_pair(
          cloud_storage_clients::error_outcome::fail, result);
    }

    if (!_remote_read_enabled) {
        result.error = "Remote read is not enabled for this cluster.";
        co_return std::make_pair(
          cloud_storage_clients::error_outcome::fail, result);
    }

    auto rtc = retry_chain_node(_opts.timeout, _opts.backoff, &_rtc);

    const auto start = ss::lowres_clock::now();
    try {
        const cloud_storage::remote::list_result object_list
          = co_await _cloud_storage_api.local().list_objects(
            bucket, rtc, std::nullopt, std::nullopt, std::nullopt, max_keys);

        if (!object_list) {
            result.error = "Failed to list objects in cloud storage.";
        }

        co_return std::make_pair(std::move(object_list), std::move(result));
    } catch (const std::exception& e) {
        result.error = e.what();
    }

    const auto end = ss::lowres_clock::now();
    result.duration = end - start;

    co_return std::make_pair(
      cloud_storage_clients::error_outcome::fail, std::move(result));
}

ss::future<std::pair<std::optional<iobuf>, self_test_result>>
cloudcheck::verify_download(
  cloud_storage_clients::bucket_name bucket,
  std::optional<cloud_storage_clients::object_key> key) {
    auto result = self_test_result{
      .name = _opts.name, .info = "download", .test_type = "cloud_storage"};

    if (_cancelled) {
        result.warning = "Run was manually cancelled.";
        co_return std::make_pair(std::nullopt, result);
    }

    if (!_remote_read_enabled) {
        result.error = "Remote read is not enabled for this cluster.";
        co_return std::make_pair(std::nullopt, result);
    }

    if (!key) {
        result.warning = "Could not download from cloud storage (no file was "
                         "found in the bucket).";
        co_return std::make_pair(std::nullopt, result);
    }

    iobuf download_payload;
    auto rtc = retry_chain_node(_opts.timeout, _opts.backoff, &_rtc);
    cloud_storage::download_request download_request = make_download_request(
      bucket, key.value(), std::ref(download_payload), rtc);

    const auto start = ss::lowres_clock::now();
    try {
        const cloud_storage::download_result download_result
          = co_await _cloud_storage_api.local().download_object(
            std::move(download_request));

        std::optional<iobuf> result_payload;
        switch (download_result) {
        case cloud_storage::download_result::success:
            result_payload = std::move(download_payload);
            break;
        case cloud_storage::download_result::timedout:
            [[fallthrough]];
        case cloud_storage::download_result::failed:
            [[fallthrough]];
        case cloud_storage::download_result::notfound:
            result.error = "Failed to download from cloud storage.";
            break;
        }

        co_return std::make_pair(std::move(result_payload), std::move(result));
    } catch (const std::exception& e) {
        result.error = e.what();
    }

    const auto end = ss::lowres_clock::now();
    result.duration = end - start;

    co_return std::make_pair(std::nullopt, std::move(result));
}

ss::future<self_test_result> cloudcheck::verify_delete(
  cloud_storage_clients::bucket_name bucket,
  cloud_storage_clients::object_key key) {
    auto result = self_test_result{
      .name = _opts.name, .info = "delete", .test_type = "cloud_storage"};

    if (_cancelled) {
        result.warning = "Run was manually cancelled.";
        co_return result;
    }

    if (!_remote_write_enabled) {
        result.error = "Remote write is not enabled for this cluster.";
        co_return result;
    }

    auto rtc = retry_chain_node(_opts.timeout, _opts.backoff, &_rtc);

    const auto start = ss::lowres_clock::now();
    try {
        const cloud_storage::upload_result delete_result
          = co_await _cloud_storage_api.local().delete_object(bucket, key, rtc);

        switch (delete_result) {
        case cloud_storage::upload_result::success:
            break;
        case cloud_storage::upload_result::timedout:
            [[fallthrough]];
        case cloud_storage::upload_result::failed:
            [[fallthrough]];
        case cloud_storage::upload_result::cancelled:
            result.error = "Failed to delete from cloud storage.";
            break;
        }
    } catch (const std::exception& e) {
        result.error = e.what();
    }

    const auto end = ss::lowres_clock::now();
    result.duration = end - start;

    co_return result;
}

} // namespace cluster::self_test
