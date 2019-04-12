#include "server.h"

#include <sys/utsname.h>
#include <sys/resource.h>
#include <glog/logging.h>
#include <utility>

#include "util.h"
#include "worker.h"
#include "version.h"
#include "redis_request.h"

Server::Server(Engine::Storage *storage, Config *config) :
  storage_(storage), config_(config) {
  for (int i = 0; i < config->workers; i++) {
    auto worker = new Worker(this, config);
    worker_threads_.emplace_back(new WorkerThread(worker));
  }
  for (int i = 0; i < config->repl_workers; i++) {
    auto repl_worker = new Worker(this, config, true);
    worker_threads_.emplace_back(new WorkerThread(repl_worker));
  }
  task_runner_ = new TaskRunner(2, 1024);
  time(&start_time_);
}

Server::~Server() {
  for (const auto &worker_thread : worker_threads_) {
    delete worker_thread;
  }
  delete task_runner_;
}

Status Server::Start() {
  if (!config_->master_host.empty()) {
    Status s = AddMaster(config_->master_host, static_cast<uint32_t>(config_->master_port));
    if (!s.IsOK()) return s;
  }

  for (const auto worker : worker_threads_) {
    worker->Start();
  }
  task_runner_->Start();
  // setup server cron thread
  cron_thread_ = std::thread([this]() {
    Util::ThreadSetName("server-cron");
    this->cron();
  });
  return Status::OK();
}

void Server::Stop() {
  stop_ = true;
  if (replication_thread_) replication_thread_->Stop();
  for (const auto worker : worker_threads_) {
    worker->Stop();
  }
  task_runner_->Stop();
  task_runner_->Join();
  if (cron_thread_.joinable()) cron_thread_.join();
}

void Server::Join() {
  for (const auto worker : worker_threads_) {
    worker->Join();
  }
}

Status Server::AddMaster(std::string host, uint32_t port) {
  slaveof_mu_.lock();
  if (!master_host_.empty() && master_host_ == host && master_port_ == port) {
    slaveof_mu_.unlock();
    return Status::OK();
  }

  if (!master_host_.empty()) {
    if (replication_thread_) replication_thread_->Stop();
    replication_thread_ = nullptr;
  }
  master_host_ = std::move(host);
  master_port_ = port;
  replication_thread_ = std::unique_ptr<ReplicationThread>(
      new ReplicationThread(master_host_, master_port_, storage_, config_->masterauth));
  replication_thread_->Start([this]() { this->is_loading_ = true; },
                             [this]() { this->is_loading_ = false; });
  slaveof_mu_.unlock();
  return Status::OK();
}

Status Server::RemoveMaster() {
  slaveof_mu_.lock();
  if (!master_host_.empty()) {
    master_host_.clear();
    master_port_ = 0;
    if (replication_thread_) replication_thread_->Stop();
    replication_thread_ = nullptr;
  }
  slaveof_mu_.unlock();
  return Status::OK();
}

int Server::PublishMessage(const std::string &channel, const std::string &msg) {
  int cnt = 0;

  auto iter = pubsub_channels_.find(channel);
  if (iter == pubsub_channels_.end()) {
    return 0;
  }
  std::string reply;
  reply.append(Redis::MultiLen(3));
  reply.append(Redis::BulkString("message"));
  reply.append(Redis::BulkString(channel));
  reply.append(Redis::BulkString(msg));
  for (const auto conn : iter->second) {
    Redis::Reply(conn->Output(), reply);
    cnt++;
  }
  return cnt;
}

void Server::SubscribeChannel(const std::string &channel, Redis::Connection *conn) {
  auto iter = pubsub_channels_.find(channel);
  if (iter == pubsub_channels_.end()) {
    std::list<Redis::Connection*> conns;
    conns.emplace_back(conn);
    pubsub_channels_.insert(std::pair<std::string, std::list<Redis::Connection*>>(channel, conns));
  } else {
    iter->second.emplace_back(conn);
  }
}

void Server::UnSubscribeChannel(const std::string &channel, Redis::Connection *conn) {
  auto iter = pubsub_channels_.find(channel);
  if (iter == pubsub_channels_.end()) {
    return;
  }
  for (const auto &c : iter->second) {
    if (conn == c) {
      iter->second.remove(c);
      break;
    }
  }
}

Status Server::IncrClients() {
  auto connections = connected_clients_.fetch_add(1, std::memory_order_relaxed);
  if (config_->maxclients > 0 && connections >= config_->maxclients) {
    connected_clients_.fetch_sub(1, std::memory_order_relaxed);
    return Status(Status::NotOK, "max number of clients reached");
  }
  total_clients_.fetch_add(1, std::memory_order_relaxed);
  return Status::OK();
}

void Server::DecrClients() {
  connected_clients_.fetch_sub(1, std::memory_order_relaxed);
}

void Server::clientsCron() {
  if (config_->timeout <= 0) return;
  KickoutIdleClients();
}

std::atomic<uint64_t> *Server::GetClientID() {
  return &client_id_;
}

Status Server::compactCron() {
  Status s = AsyncCompactDB();
  if (!s.IsOK()) return s;
  LOG(INFO) << "Commpact was triggered by cron with executed success.";
  return Status::OK();
}

Status Server::bgsaveCron() {
  if (this->IsSlave()) {
    return Status::OK();  // Don't let slave do any bgsave
  }
  Status s = AsyncBgsaveDB();
  if (!s.IsOK()) return s;
  LOG(INFO) << "Bgsave was triggered by cron with executed success.";
  return Status::OK();
}

void Server::cron() {
  static uint64_t counter = 0;
  std::time_t t;
  std::tm *now;
  while (!stop_) {
    if (counter != 0 && counter % 10000 == 0) {
      clientsCron();
    }
    if (counter != 0 && counter % 60000 == 0) {
      storage_->PurgeOldBackups(config_->max_backup_to_keep);
    }
    // check every 1 minute
    if (counter != 0 && counter % 60000 == 0) {
      if (config_->compact_cron.IsEnabled()) {
        t = std::time(0);
        now = std::localtime(&t);
        if (config_->compact_cron.IsTimeMatch(now)) {
          compactCron();
        }
      }
      if (config_->bgsave_cron.IsEnabled()) {
        t = std::time(0);
        now = std::localtime(&t);
        if (config_->bgsave_cron.IsTimeMatch(now)) {
          bgsaveCron();
        }
      }
    }
    counter++;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void Server::GetRocksDBInfo(std::string *info) {
  std::ostringstream string_stream;
  rocksdb::DB *db = storage_->GetDB();

  uint64_t metadata_estimate_keys, subkey_estimate_keys, score_estimate_keys;
  uint64_t memtable_sizes, num_snapshots, num_running_flushes;
  uint64_t num_immutable_tables, memtable_flush_pending, compaction_pending;
  uint64_t num_running_compaction, num_live_versions, num_superversion, num_backgroud_errors;

  db->GetIntProperty(storage_->GetCFHandle("default"), "rocksdb.estimate-num-keys", &subkey_estimate_keys);
  db->GetIntProperty(storage_->GetCFHandle("zset_score"), "rocksdb.estimate-num-keys", &score_estimate_keys);
  db->GetIntProperty(storage_->GetCFHandle("metadata"), "rocksdb.estimate-num-keys", &metadata_estimate_keys);
  db->GetAggregatedIntProperty("rocksdb.num-snapshots", &num_snapshots);
  db->GetAggregatedIntProperty("rocksdb.size-all-mem-tables", &memtable_sizes);
  db->GetAggregatedIntProperty("rocksdb.num-running-flushes", &num_running_flushes);
  db->GetAggregatedIntProperty("rocksdb.num-immutable-mem-table", &num_immutable_tables);
  db->GetAggregatedIntProperty("rocksdb.mem-table-flush-pending", &memtable_flush_pending);
  db->GetAggregatedIntProperty("rocksdb.num-running-compactions", &num_running_compaction);
  db->GetAggregatedIntProperty("rocksdb.current-super-version-number", &num_superversion);
  db->GetAggregatedIntProperty("rocksdb.background-errors", &num_backgroud_errors);
  db->GetAggregatedIntProperty("rocksdb.compaction-pending", &compaction_pending);
  db->GetAggregatedIntProperty("rocksdb.num-live-versions", &num_live_versions);

  string_stream << "# RocksDB\r\n";
  string_stream << "estimate_keys:" << metadata_estimate_keys << "\r\n";
  string_stream << "estimate_keys[subkey]:" << subkey_estimate_keys << "\r\n";
  string_stream << "estimate_keys[score]:" << score_estimate_keys << "\r\n";
  string_stream << "all_mem_tables:" << memtable_sizes << "\r\n";
  string_stream << "snapshots:" << num_snapshots << "\r\n";
  string_stream << "num_immutable_tables:" << num_immutable_tables << "\r\n";
  string_stream << "num_running_flushes:" << num_running_flushes << "\r\n";
  string_stream << "memtable_flush_pending:" << memtable_flush_pending << "\r\n";
  string_stream << "compaction_pending:" << compaction_pending << "\r\n";
  string_stream << "num_running_compactions:" << num_running_compaction << "\r\n";
  string_stream << "num_live_versions:" << num_live_versions << "\r\n";
  string_stream << "num_superversion:" << num_superversion << "\r\n";
  string_stream << "num_background_errors:" << num_backgroud_errors << "\r\n";
  *info = string_stream.str();
}

void Server::GetServerInfo(std::string *info) {
  time_t now;
  std::ostringstream string_stream;
  static int call_uname = 1;
  static utsname name;
  if (call_uname) {
    /* Uname can be slow and is always the same output. Cache it. */
    uname(&name);
    call_uname = 0;
  }
  time(&now);
  string_stream << "# Server\r\n";
  string_stream << "version:" << VERSION << "\r\n";
  string_stream << "git_sha1:" << GIT_COMMIT << "\r\n";
  string_stream << "os:" << name.sysname << " " << name.release << " " << name.machine << "\r\n";
#ifdef __GNUC__
  string_stream << "gcc_version:" << __GNUC__ << "." << __GNUC_MINOR__ << "." << __GNUC_PATCHLEVEL__ << "\r\n";
#else
  string_stream << "gcc_version:0,0,0\r\n";
#endif
  string_stream << "arch_bits:" << sizeof(void *) * 8 << "\r\n";
  string_stream << "process_id:" << getpid() << "\r\n";
  string_stream << "tcp_port:" << config_->port << "\r\n";
  string_stream << "uptime_in_seconds:" << now-start_time_ << "\r\n";
  string_stream << "uptime_in_days:" << (now-start_time_)/86400 << "\r\n";
  *info = string_stream.str();
}

void Server::GetClientsInfo(std::string *info) {
  std::ostringstream string_stream;
  string_stream << "# Clients\r\n";
  string_stream << "connected_clients:" << connected_clients_ << "\r\n";
  *info = string_stream.str();
}

void Server::GetMemoryInfo(std::string *info) {
  std::ostringstream string_stream;
  char buf[16];
  int64_t rss = Stats::GetMemoryRSS();
  Util::BytesToHuman(buf, 16, static_cast<uint64_t>(rss));
  string_stream << "# Memory\r\n";
  string_stream << "used_memory_rss:" << rss <<"\r\n";
  string_stream << "used_memory_human:" << buf <<"\r\n";
  *info = string_stream.str();
}

void Server::GetReplicationInfo(std::string *info) {
  time_t now;
  std::ostringstream string_stream;
  string_stream << "# Replication\r\n";
  if (IsSlave()) {
    time(&now);
    string_stream << "role: slave\r\n";
    string_stream << "master_host:" << master_host_ << "\r\n";
    string_stream << "master_port:" << master_port_ << "\r\n";
    ReplState state = replication_thread_->State();
    string_stream << "master_link_status:" << (state == kReplConnected? "up":"down") << "\r\n";
    string_stream << "master_sync_unrecoverable_error:" << (state == kReplError ? "yes" : "no") << "\r\n";
    string_stream << "master_sync_in_progress:" << (state == kReplFetchMeta || state == kReplFetchSST) << "\r\n";
    string_stream << "master_last_io_seconds_ago:" << now-replication_thread_->LastIOTime() << "\r\n";
  } else {
    string_stream << "role: master\r\n";
    int idx = 0;
    rocksdb::SequenceNumber latest_seq = storage_->LatestSeq();
    for (const auto &slave_info : slaves_info_) {
      string_stream << "slave_" << std::to_string(idx) << ":";
      string_stream << "addr=" << slave_info->addr
                    << ",port=" << slave_info->port
                    << ",seq=" << slave_info->seq
                    << ",lag=" << latest_seq - slave_info->seq << "\r\n";
      ++idx;
    }
  }
  *info = string_stream.str();
}

void Server::GetStatsInfo(std::string *info) {
  std::ostringstream string_stream;
  string_stream << "# Stats\r\n";
  string_stream << "total_connections_received:" << total_clients_ <<"\r\n";
  string_stream << "total_commands_processed:" << stats_.total_calls <<"\r\n";
  string_stream << "total_net_input_bytes:" << stats_.in_bytes <<"\r\n";
  string_stream << "total_net_output_bytes:" << stats_.out_bytes <<"\r\n";
  string_stream << "sync_full:" << stats_.fullsync_counter <<"\r\n";
  string_stream << "sync_partial_ok:" << stats_.psync_ok_counter <<"\r\n";
  string_stream << "sync_partial_err:" << stats_.psync_err_counter <<"\r\n";
  string_stream << "pubsub_channels:" << pubsub_channels_.size() <<"\r\n";
  *info = string_stream.str();
}

void Server::GetCommandsStatsInfo(std::string *info) {
  std::ostringstream string_stream;
  string_stream << "# Commandstats\r\n";

  for (const auto &cmd_stat : stats_.commands_stats) {
    auto calls = cmd_stat.second.calls.load();
    auto latency = cmd_stat.second.latency.load();
    string_stream << "cmdstat_" << cmd_stat.first << ":calls=" << calls
                  << ",usec=" << latency << ",usec_per_call="
                  << ((calls == 0) ? 0 : static_cast<float>(latency/calls))
                  << "\r\n";
  }
  *info = string_stream.str();
}

void Server::GetInfo(const std::string &ns, const std::string &section, std::string *info) {
  info->clear();
  std::ostringstream string_stream;
  bool all = section == "all";

  if (all || section == "server") {
    std::string server_info;
    GetServerInfo(&server_info);
    string_stream << server_info;
  }
  if (all || section == "clients") {
    std::string clients_info;
    GetClientsInfo(&clients_info);
    string_stream << clients_info;
  }
  if (all || section == "memory") {
    std::string memory_info;
    GetMemoryInfo(&memory_info);
    string_stream << memory_info;
  }
  if (all || section == "persistence") {
    string_stream << "# Persistence\r\n";
    string_stream << "loading:" << is_loading_ <<"\r\n";
  }
  if (all || section == "stats") {
    std::string stats_info;
    GetStatsInfo(&stats_info);
    string_stream << stats_info;
  }
  if (all || section == "replication") {
    std::string replication_info;
    GetReplicationInfo(&replication_info);
    string_stream << replication_info;
  }
  if (all || section == "cpu") {
    struct rusage self_ru;
    getrusage(RUSAGE_SELF, &self_ru);
    string_stream << "# CPU\r\n";
    string_stream << "used_cpu_sys:"
                  << static_cast<float>(self_ru.ru_stime.tv_sec)+static_cast<float>(self_ru.ru_stime.tv_usec/1000000)
                  << "\r\n";
    string_stream << "used_cpu_user:"
                  << static_cast<float>(self_ru.ru_utime.tv_sec)+static_cast<float>(self_ru.ru_utime.tv_usec/1000000)
                  << "\r\n";
  }
  if (all || section == "commandstats") {
    std::string commands_stats_info;
    GetCommandsStatsInfo(&commands_stats_info);
    string_stream << commands_stats_info;
  }
  if (all || section == "keyspace") {
    time_t last_scan_time = GetLastScanTime(ns);
    string_stream << "# Keyspace\r\n";
    string_stream << "# Last scan db time: " << std::asctime(std::localtime(&last_scan_time));
    string_stream << "dbsize: " << GetLastKeyNum(ns) << "\r\n";
    string_stream << "sequence: " << storage_->GetDB()->GetLatestSequenceNumber() << "\r\n";
  }
  if (all || section == "rocksdb") {
    std::string rocksdb_info;
    GetRocksDBInfo(&rocksdb_info);
    string_stream << rocksdb_info;
  }
  *info = string_stream.str();
}

std::string Server::GetRocksDBStatsJson() {
  char buf[256];
  std::string output;

  output.reserve(8*1024);
  output.append("{");
  auto stats = storage_->GetDB()->GetDBOptions().statistics;
  for (const auto &iter : rocksdb::TickersNameMap) {
    snprintf(buf, sizeof(buf), "\"%s\":%" PRIu64 ",",
             iter.second.c_str(), stats->getTickerCount(iter.first));
    output.append(buf);
  }
  for (const auto &iter : rocksdb::HistogramsNameMap) {
    rocksdb::HistogramData hist_data;
    stats->histogramData(iter.first, &hist_data);
    /* P50 P95 P99 P100 COUNT SUM */
    snprintf(buf, sizeof(buf), "\"%s\":[%f,%f,%f,%f,%" PRIu64 ",%" PRIu64 "],",
             iter.second.c_str(),
             hist_data.median, hist_data.percentile95, hist_data.percentile99,
             hist_data.max, hist_data.count, hist_data.sum);
    output.append(buf);
  }
  output.pop_back();
  output.append("}");
  output.shrink_to_fit();
  return output;
}

Status Server::AsyncCompactDB() {
  db_mu_.lock();
  if (db_compacting_) {
    db_mu_.unlock();
    return Status(Status::NotOK, "compacting the db now");
  }
  db_compacting_ = true;
  db_mu_.unlock();

  Task task;
  task.arg = this;
  task.callback = [](void *arg) {
    auto svr = static_cast<Server*>(arg);
    svr->storage_->Compact(nullptr, nullptr);
    svr->db_mu_.lock();
    svr->db_compacting_ = false;
    svr->db_mu_.unlock();
  };
  return task_runner_->Publish(task);
}

Status Server::AsyncBgsaveDB() {
  db_mu_.lock();
  if (db_bgsave_) {
    db_mu_.unlock();
    return Status(Status::NotOK, "bgsave in-progress");
  }
  db_bgsave_ = true;
  db_mu_.unlock();

  Task task;
  task.arg = this;
  task.callback = [](void *arg) {
    auto svr = static_cast<Server*>(arg);
    svr->storage_->CreateBackup();
    svr->db_mu_.lock();
    svr->db_bgsave_ = false;
    svr->db_mu_.unlock();
  };
  return task_runner_->Publish(task);
}

Status Server::AsyncScanDBSize(const std::string &ns) {
  db_mu_.lock();
  auto iter = db_scan_infos_.find(ns);
  if (iter == db_scan_infos_.end()) {
    db_scan_infos_[ns] = DBScanInfo{};
  }
  if (db_scan_infos_[ns].is_scanning) {
    db_mu_.unlock();
    return Status(Status::NotOK, "scanning the db now");
  }
  db_scan_infos_[ns].is_scanning = true;
  db_mu_.unlock();

  Task task;
  task.arg = this;
  task.callback = [ns](void *arg) {
    auto svr = static_cast<Server*>(arg);
    RedisDB db(svr->storage_, ns);
    uint64_t key_num = db.GetKeyNum();

    svr->db_mu_.lock();
    svr->db_scan_infos_[ns].n_key = key_num;
    time(&svr->db_scan_infos_[ns].last_scan_time);
    svr->db_scan_infos_[ns].is_scanning = false;
    svr->db_mu_.unlock();
  };
  return task_runner_->Publish(task);
}

uint64_t Server::GetLastKeyNum(const std::string &ns) {
  auto iter = db_scan_infos_.find(ns);
  if (iter != db_scan_infos_.end()) {
    return iter->second.n_key;
  }
  return 0;
}

time_t Server::GetLastScanTime(const std::string &ns) {
  auto iter = db_scan_infos_.find(ns);
  if (iter != db_scan_infos_.end()) {
    return iter->second.last_scan_time;
  }
  return 0;
}

void Server::SlowlogReset() {
  slowlog_.mu.lock();
  slowlog_.entry_list.clear();
  slowlog_.mu.unlock();
}

uint Server::SlowlogLen() {
  std::unique_lock<std::mutex> lock(slowlog_.mu);
  return slowlog_.entry_list.size();
}

void Server::CreateSlowlogReply(std::string *output, uint32_t count) {
  uint32_t sent = 0;
  slowlog_.mu.lock();
  for (auto iter = slowlog_.entry_list.begin(); iter != slowlog_.entry_list.end() && sent < count; ++iter) {
    sent++;
    output->append(Redis::MultiLen(4));
    output->append(Redis::Integer(iter->id));
    output->append(Redis::Integer(iter->time));
    output->append(Redis::Integer(iter->duration));
    output->append(Redis::MultiBulkString(iter->args));
  }
  output->insert(0, Redis::MultiLen(sent));
  slowlog_.mu.unlock();
}

void Server::SlowlogPushEntryIfNeeded(const std::vector<std::string>* args, uint64_t duration) {
  if (config_->slowlog_log_slower_than < 0) return;
  if (static_cast<int64_t>(duration) < config_->slowlog_log_slower_than) return;
  slowlog_.mu.lock();
  slowlog_.entry_list.emplace_front(SlowlogEntry{*args, ++slowlog_.id, duration, time(nullptr)});

  while (slowlog_.entry_list.size() > config_->slowlog_max_len) {
    slowlog_.entry_list.pop_back();
  }
  slowlog_.mu.unlock();
}

std::string Server::GetClientsStr() {
  std::string clients;
  for (const auto worker : worker_threads_) {
    clients.append(worker->GetClientsStr());
  }
  return clients;
}

void Server::KillClient(int64_t *killed, std::string addr, uint64_t id, bool skipme, Redis::Connection *conn) {
  for (const auto worker : worker_threads_) {
    worker->KillClient(killed, addr, id, skipme, conn);
  }
}

void Server::KickoutIdleClients() {
  for (const auto worker : worker_threads_) {
    worker->KickoutIdleClients(config_->timeout);
  }
}

Server::SlaveInfoPos Server::AddSlave(const std::string &addr, uint32_t port) {
  std::lock_guard<std::mutex> guard(slaves_info_mu_);
  slaves_info_.push_back(std::shared_ptr<SlaveInfo>(new SlaveInfo(addr, port)));
  return --(slaves_info_.end());
}

void Server::RemoveSlave(const SlaveInfoPos &pos) {
  std::lock_guard<std::mutex> guard(slaves_info_mu_);
  slaves_info_.erase(pos);
}

void Server::UpdateSlaveStats(const SlaveInfoPos &pos, rocksdb::SequenceNumber seq) {
  (*pos)->seq = seq;
}
