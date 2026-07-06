#pragma once

// test-only postgres launcher, shells out to pg cli tools. do not add to core
// lib.

#include <cstdlib>
#include <fstream>
#include <spdlog/spdlog.h>
#include <string>

namespace testutil {

class PgLauncher {
public:
  PgLauncher(std::string installDir, std::string dataDir)
      : installDir_(std::move(installDir)), dataDir_(std::move(dataDir)) {}

  ~PgLauncher() {
    if (started_) {
      run(installDir_ + "/bin/pg_ctl -D " + dataDir_ + " -m fast -w stop");
    }
  }

  bool init() {
    return run(installDir_ + "/bin/initdb -D " + dataDir_ + " -A trust");
  }

  void add_config(std::string const &key, std::string const &value) {
    if (key == "port") {
      port_ = value;
    }
    std::ofstream out(dataDir_ + "/postgresql.conf", std::ios_base::app);
    out << key << " = " << value << "\n";
  }

  bool start() {
    started_ = run(installDir_ + "/bin/pg_ctl -D " + dataDir_ + " -l " +
                   dataDir_ + "/server.log -w start");
    return started_;
  }

  bool createdb(std::string const &name) {
    return run(installDir_ + "/bin/createdb -h 127.0.0.1 -p " + port_ + " " +
               name);
  }

  bool createuser(std::string const &name, std::string const &args) {
    return run(installDir_ + "/bin/createuser -h 127.0.0.1 -p " + port_ + " " +
               args + " " + name);
  }

private:
  bool run(std::string const &cmd) {
    spdlog::info("running: {}", cmd);
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
      spdlog::error("command failed (code {}): {}", rc, cmd);
      return false;
    }
    return true;
  }

  std::string installDir_;
  std::string dataDir_;
  std::string port_;
  bool started_ = false;
};

} // namespace testutil
