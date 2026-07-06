#pragma once

// test-only mysql launcher, shells out to mysql cli tools. do not add to core
// lib.

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>

namespace testutil {

class MysqlLauncher {
public:
  MysqlLauncher(std::string installDir, std::string dataDir)
      : installDir_(std::move(installDir)), dataDir_(std::move(dataDir)) {}

  ~MysqlLauncher() {
    if (started_) {
      run(client("mysqladmin") + " -uroot --socket=" + sock() + " shutdown");
    }
  }

  bool init() {
    return run(server() + " --no-defaults --initialize-insecure --datadir=" +
               dataDir_ + "/data --log-error=" + dataDir_ + "/init.err");
  }

  void add_config(std::string const &key, std::string const &value) {
    if (key == "port") {
      port_ = value;
    }
    extraArgs_ += " --" + key + "=" + value;
  }

  bool start() {
    started_ =
        run(server() + " --no-defaults --datadir=" + dataDir_ +
            "/data --port=" + port_ + " --socket=" + sock() +
            " --log-error=" + dataDir_ + "/server.err --pid-file=" + dataDir_ +
            "/mysqld.pid" + extraArgs_ + " &");
    for (int i = 0; started_ && i < 300; ++i) {
      if (run(client("mysqladmin") + " -uroot --socket=" + sock() +
              " ping >/dev/null 2>&1")) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
  }

  bool createdb(std::string const &name) {
    return sql("CREATE DATABASE " + name);
  }

  bool createuser(std::string const &name, std::string const & /*args*/) {
    return sql("CREATE USER '" + name + "'@'%'") &&
           sql("GRANT ALL ON *.* TO '" + name + "'@'%' WITH GRANT OPTION");
  }

private:
  bool sql(std::string const &stmt) {
    return run(client("mysql") + " -uroot --socket=" + sock() + " -e \"" +
               stmt + "\"");
  }

  std::string server() const {
    std::string bin = installDir_ + "/bin/mysqld";
    std::ifstream probe(bin);
    return probe.good() ? bin : installDir_ + "/sbin/mysqld";
  }

  std::string client(std::string const &name) const {
    return installDir_ + "/bin/" + name;
  }

  std::string sock() const { return dataDir_ + "/mysql.sock"; }

  bool run(std::string const &cmd) {
    spdlog::info("Running: {}", cmd);
    return std::system(cmd.c_str()) == 0;
  }

  std::string installDir_;
  std::string dataDir_;
  std::string port_ = "3306";
  std::string extraArgs_;
  bool started_ = false;
};

} // namespace testutil
