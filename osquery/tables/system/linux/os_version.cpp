/*
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <map>
#include <string>

#include <boost/xpressive/xpressive.hpp>

#include <osquery/filesystem.h>
#include <osquery/sql.h>
#include <osquery/tables.h>

#include "osquery/core/conversions.h"

namespace xp = boost::xpressive;

namespace osquery {
namespace tables {

const std::string kOSRelease = "/etc/os-release";
const std::string kRedhatRelease = "/etc/redhat-release";

const std::map<std::string, std::string> kOSReleaseColumns = {
    {"NAME", "name"},
    {"VERSION", "version"},
    {"BUILD_ID", "build"},
    {"ID", "platform"},
    {"ID_LIKE", "platform_like"},
    {"VERSION_CODENAME", "codename"},
    {"VERSION_ID", "_id"},
};

QueryData genOSRelease() {
  // This will parse /etc/os-version according to the systemd manual.
  std::string content;
  if (!readFile(kOSRelease, content).ok()) {
    return {};
  }

  Row r;
  for (const auto& line : osquery::split(content, "\n")) {
    auto fields = osquery::split(line, "=", 1);
    if (fields.size() != 2) {
      continue;
    }

    auto column = std::ref(kOSReleaseColumns.at("VERSION_CODENAME"));
    if (kOSReleaseColumns.count(fields[0]) != 0) {
      column = std::ref(kOSReleaseColumns.at(fields[0]));
    } else if (fields[0].find("CODENAME") == std::string::npos) {
      // Some distros may attach/invent their own CODENAME field.
      continue;
    }

    r[column] = std::move(fields[1]);
    if (!r.at(column).empty() && r.at(column)[0] == '"') {
      // This is quote-enclosed string, make it pretty!
      r[column] = r[column].substr(1, r.at(column).size() - 2);
    }

    if (column.get() == "_id") {
      auto parts = osquery::split(r.at(column), ".", 2);
      switch (parts.size()) {
      case 3:
        r["patch"] = parts[2];
      case 2:
        r["minor"] = parts[1];
      case 1:
        r["major"] = parts[0];
        break;
      }
    }
  }

  return {r};
}

QueryData genOSVersion(QueryContext& context) {
  if (isReadable(kOSRelease)) {
    return genOSRelease();
  }

  std::string content;
  if (!isReadable(kRedhatRelease) || !readFile(kRedhatRelease, content).ok()) {
    // This is an unknown Linux OS.
    return {};
  }

  Row r;
  // This is an older version of a Redhat-based OS.
  auto rx = xp::sregex::compile(
      "(?P<name>[\\w+\\s]+) .* "
      "(?P<major>[0-9]+)\\.(?P<minor>[0-9]+)\\.?(?P<patch>\\w+)?");
  xp::smatch matches;
  for (const auto& line : osquery::split(content, "\n")) {
    if (xp::regex_search(line, matches, rx)) {
      r["major"] = INTEGER(matches["major"]);
      r["minor"] = INTEGER(matches["minor"]);
      r["patch"] =
          (matches["patch"].length() > 0) ? INTEGER(matches["patch"]) : "0";
      r["name"] = matches["name"];
      break;
    }
  }

  r["version"] = content;
  r["platform_like"] = "rhel";

  // No build name.
  r["build"] = "";
  return {r};
}
}
}
