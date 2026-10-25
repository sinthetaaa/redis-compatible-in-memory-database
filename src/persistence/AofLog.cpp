#include "redisdb/persistence/AofLog.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace redisdb::persistence {
namespace {
constexpr std::string_view Magic = "RDBAOF01";
constexpr std::size_t FileHeaderBytes = 20;
constexpr std::size_t RecordHeaderBytes = 16;
constexpr std::size_t MaxItems = 1024 * 1024;
constexpr std::size_t MaxBlobBytes = 8 * 1024 * 1024;

[[noreturn]] void io_error(const char* operation) {
    throw PersistenceError(std::string("AOF ") + operation + ": " +
                           std::error_code(errno, std::generic_category()).message());
}
[[noreturn]] void invalid(const char* why) { throw PersistenceError(std::string("AOF ") + why); }

storage::WallTimePoint deadline_from_micros(std::int64_t micros) {
    using Duration = storage::WallTimePoint::duration;
    static_assert(Duration::period::num == 1 && Duration::period::den % 1000000 == 0);
    constexpr std::int64_t scale = Duration::period::den / 1000000;
    if (micros > std::numeric_limits<std::int64_t>::max() / scale ||
        micros < std::numeric_limits<std::int64_t>::min() / scale) {
        invalid("deadline is outside this platform's clock range");
    }
    return storage::WallTimePoint{Duration{micros * scale}};
}

class Writer {
  public:
    std::string bytes;
    void number(std::uint64_t value, unsigned width) {
        room(width);
        for (unsigned i = width; i != 0; --i) {
            bytes.push_back(static_cast<char>((value >> ((i - 1) * 8)) & 255U));
        }
    }
    void blob(std::string_view value) {
        if (value.size() > MaxBlobBytes) {
            invalid("blob exceeds 8 MiB format limit");
        }
        number(value.size(), 4);
        room(value.size());
        bytes.append(value);
    }
    void count(std::size_t value) {
        if (value > MaxItems) {
            invalid("item count exceeds format limit");
        }
        number(value, 4);
    }

  private:
    void room(std::size_t amount) {
        if (amount > MaxAofRecordBytes - bytes.size()) {
            invalid("transaction exceeds 64 MiB format limit");
        }
    }
};

class Reader {
  public:
    explicit Reader(std::string_view data) : data_(data) {}
    std::uint64_t number(unsigned width) {
        if (remaining() < width) {
            invalid("truncated payload");
        }
        std::uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) {
            value = (value << 8) | static_cast<unsigned char>(data_[pos_++]);
        }
        return value;
    }
    std::string blob() {
        const auto length = number(4);
        if (length > MaxBlobBytes || length > remaining()) {
            invalid("invalid blob length");
        }
        std::string result(data_.substr(pos_, static_cast<std::size_t>(length)));
        pos_ += static_cast<std::size_t>(length);
        return result;
    }
    std::size_t count() {
        const auto n = number(4);
        if (n > MaxItems || n > remaining() / 4) {
            invalid("invalid item count");
        }
        return static_cast<std::size_t>(n);
    }
    std::size_t remaining() const noexcept { return data_.size() - pos_; }

  private:
    std::string_view data_;
    std::size_t pos_ = 0;
};

std::string read_at(int fd, std::size_t length, off_t offset) {
    std::string result(length, '\0');
    std::size_t done = 0;
    while (done < length) {
        const auto n =
            ::pread(fd, result.data() + done, length - done, offset + static_cast<off_t>(done));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            io_error("read");
        }
        if (n == 0) {
            invalid("unexpected EOF; file left unchanged");
        }
        done += static_cast<std::size_t>(n);
    }
    return result;
}
} // namespace

std::uint32_t crc32c(std::string_view bytes) noexcept {
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto n = i;
            for (int bit = 0; bit < 8; ++bit) {
                n = (n >> 1) ^ ((n & 1U) ? 0x82f63b78U : 0U);
            }
            values[i] = n;
        }
        return values;
    }();
    std::uint32_t crc = 0xffffffffU;
    for (const char raw : bytes) {
        const auto byte = static_cast<unsigned char>(raw);
        crc = table[(crc ^ byte) & 255U] ^ (crc >> 8);
    }
    return ~crc;
}

std::string encode_effects(const storage::CommittedEffects& effects) {
    Writer out;
    out.count(effects.entries.size());
    std::vector<const storage::EntryEffect*> sorted;
    for (const auto& effect : effects.entries) {
        sorted.push_back(&effect);
    }
    std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return a->key < b->key; });
    std::optional<std::string_view> previous;
    for (const auto* effect : sorted) {
        if (previous && *previous == effect->key) {
            invalid("duplicate effect key");
        }
        previous = effect->key;
        out.blob(effect->key);
        if (!effect->replacement) {
            out.number(0, 1);
            continue;
        }
        const auto& entry = *effect->replacement;
        out.number(entry.value.index() + 1, 1);
        out.number(entry.expires_at.has_value(), 1);
        if (entry.expires_at) {
            // Unix microseconds are portable across the supported native clocks.
            // Floor on nanosecond hosts can expire <1us early, never extend TTL.
            const auto us =
                std::chrono::floor<std::chrono::microseconds>(entry.expires_at->time_since_epoch())
                    .count();
            (void)deadline_from_micros(us);
            out.number(std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(us)), 8);
        }
        std::visit(
            [&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, storage::StringValue>) {
                    out.blob(value.bytes);
                } else if constexpr (std::is_same_v<T, storage::ListValue>) {
                    out.count(value.elements.size());
                    for (const auto& item : value.elements) {
                        out.blob(item);
                    }
                } else if constexpr (std::is_same_v<T, storage::HashValue>) {
                    out.count(value.fields.size());
                    std::vector<std::string_view> fields;
                    for (const auto& [key, bytes] : value.fields) {
                        (void)bytes;
                        fields.push_back(key);
                    }
                    std::sort(fields.begin(), fields.end());
                    for (auto field : fields) {
                        out.blob(field);
                        out.blob(value.fields.at(std::string(field)));
                    }
                } else {
                    out.count(value.members.size());
                    std::vector<std::string_view> members(value.members.begin(),
                                                          value.members.end());
                    std::sort(members.begin(), members.end());
                    for (auto member : members) {
                        out.blob(member);
                    }
                }
            },
            entry.value);
    }
    out.number(effects.has_lru, 1);
    out.count(effects.lru_order.size());
    for (const auto& key : effects.lru_order) {
        out.blob(key);
    }
    return std::move(out.bytes);
}

storage::CommittedEffects decode_effects(std::string_view bytes) {
    if (bytes.size() > MaxAofRecordBytes) {
        invalid("oversized payload");
    }
    Reader in(bytes);
    storage::CommittedEffects effects;
    const auto count = in.count();
    if (count == 0) {
        invalid("empty transaction");
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto key = in.blob();
        if (!effects.entries.empty() && key <= effects.entries.back().key) {
            invalid("unordered or duplicate effect key");
        }
        const auto tag = in.number(1);
        if (tag == 0) {
            effects.entries.push_back({std::move(key), std::nullopt});
            continue;
        }
        if (tag > 4) {
            invalid("unknown value type");
        }
        storage::Entry entry;
        const auto timed = in.number(1);
        if (timed > 1) {
            invalid("invalid deadline flag");
        }
        if (timed) {
            entry.expires_at = deadline_from_micros(std::bit_cast<std::int64_t>(in.number(8)));
        }
        if (tag == 1) {
            entry.value = storage::StringValue{in.blob()};
        } else {
            const auto n = in.count();
            if (n == 0) {
                invalid("empty collection");
            }
            if (tag == 2) {
                storage::ListValue list;
                for (std::size_t j = 0; j < n; ++j) {
                    list.elements.push_back(in.blob());
                }
                entry.value = std::move(list);
            } else if (tag == 3) {
                storage::HashValue hash;
                for (std::size_t j = 0; j < n; ++j) {
                    auto field = in.blob();
                    auto value = in.blob();
                    if (!hash.fields.emplace(std::move(field), std::move(value)).second) {
                        invalid("duplicate hash field");
                    }
                }
                entry.value = std::move(hash);
            } else {
                storage::SetValue set;
                for (std::size_t j = 0; j < n; ++j) {
                    if (!set.members.insert(in.blob()).second) {
                        invalid("duplicate set member");
                    }
                }
                entry.value = std::move(set);
            }
        }
        effects.entries.push_back({std::move(key), std::move(entry)});
    }
    const auto lru = in.number(1);
    if (lru > 1) {
        invalid("invalid LRU flag");
    }
    effects.has_lru = lru != 0;
    const auto n = in.count();
    if (!effects.has_lru && n != 0) {
        invalid("LRU data without flag");
    }
    std::set<std::string> unique;
    for (std::size_t i = 0; i < n; ++i) {
        auto key = in.blob();
        if (!unique.insert(key).second) {
            invalid("duplicate LRU key");
        }
        effects.lru_order.push_back(std::move(key));
    }
    if (in.remaining() != 0) {
        invalid("trailing payload bytes");
    }
    return effects;
}

AofLog::AofLog(const AofOptions& options, const storage::Clock& clock, AofIo io)
    : clock_(clock), policy_(options.sync), io_(std::move(io)),
      next_sync_(clock.monotonic_now() + std::chrono::seconds{1}) {
    const auto path = options.file;
    auto parent = path.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    directory_.reset(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!directory_) {
        io_error("open parent directory");
    }
    bool created = true;
    fd_.reset(
        ::open(path.c_str(), O_RDWR | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!fd_ && errno == EEXIST) {
        created = false;
        // Nonblocking open prevents a FIFO/device path from hanging before
        // fstat rejects it. O_NONBLOCK has no effect on regular-file disk I/O.
        fd_.reset(::open(path.c_str(), O_RDWR | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    }
    if (!fd_) {
        io_error("open journal");
    }
    struct stat info{};
    if (::fstat(fd_.get(), &info) != 0) {
        io_error("stat journal");
    }
    if (!S_ISREG(info.st_mode)) {
        invalid("journal must be a regular file");
    }
    if (::flock(fd_.get(), LOCK_EX | LOCK_NB) != 0) {
        io_error("exclusive writer lock");
    }
    watermark_ = path.string() + ".commit";
    if (created) {
        struct stat existing{};
        if (::lstat(watermark_.c_str(), &existing) == 0 || errno != ENOENT) {
            invalid("orphan commit watermark exists; refusing to replace it");
        }
        Writer header;
        header.bytes = Magic;
        header.number(1, 4);
        header.number(0, 4);
        header.number(crc32c(header.bytes), 4);
        write_all(header.bytes);
        synchronize(fd_.get());
        publish_watermark(); // also syncs the directory containing the new journal
    } else {
        if (info.st_size < static_cast<off_t>(FileHeaderBytes)) {
            invalid("truncated file header; file left unchanged");
        }
        const auto header = read_at(fd_.get(), FileHeaderBytes, 0);
        Reader fields(std::string_view(header).substr(8));
        if (header.substr(0, 8) != Magic || fields.number(4) != 1 || fields.number(4) != 0 ||
            fields.number(4) != crc32c(std::string_view(header).substr(0, 16))) {
            invalid("invalid file magic, version, flags or header checksum");
        }
        net::UniqueFd marker(
            ::open(watermark_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        struct stat mark_info{};
        if (!marker || ::fstat(marker.get(), &mark_info) != 0 || !S_ISREG(mark_info.st_mode) ||
            mark_info.st_size != 32) {
            invalid("missing or invalid commit watermark; files left unchanged");
        }
        const auto mark = read_at(marker.get(), 32, 0);
        Reader fields_mark(std::string_view(mark).substr(8));
        durable_sequence_ = fields_mark.number(8);
        durable_offset_ = fields_mark.number(8);
        durable_crc_ = static_cast<std::uint32_t>(fields_mark.number(4));
        if (mark.substr(0, 8) != "RDBACM01" ||
            fields_mark.number(4) != crc32c(std::string_view(mark).substr(0, 28)) ||
            durable_offset_ < FileHeaderBytes ||
            durable_offset_ > static_cast<std::uint64_t>(info.st_size) ||
            (durable_sequence_ == 0 && (durable_offset_ != FileHeaderBytes || durable_crc_ != 0))) {
            invalid("commit watermark mismatch or committed suffix missing; files left unchanged");
        }
    }
}

void AofLog::write_all(std::string_view bytes, int fd) {
    if (fd < 0) {
        fd = fd_.get();
    }
    while (!bytes.empty()) {
        const auto n = io_.write ? io_.write(fd, bytes.data(), bytes.size())
                                 : ::write(fd, bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            io_error("write");
        }
        if (n == 0 || static_cast<std::size_t>(n) > bytes.size()) {
            invalid("write made invalid progress");
        }
        bytes.remove_prefix(static_cast<std::size_t>(n));
    }
}

void AofLog::publish_watermark() {
    Writer marker;
    marker.bytes = "RDBACM01";
    marker.number(sequence_, 8);
    marker.number(end_offset_, 8);
    marker.number(last_crc_, 4);
    marker.number(crc32c(marker.bytes), 4);
    std::string temporary = watermark_.string() + ".tmp.XXXXXX";
    net::UniqueFd staging(::mkstemp(temporary.data()));
    if (!staging) {
        io_error("create commit watermark staging file");
    }
    try {
        write_all(marker.bytes, staging.get());
        synchronize(staging.get());
        if (::rename(temporary.c_str(), watermark_.c_str()) != 0) {
            io_error("publish commit watermark");
        }
        synchronize(directory_.get());
        durable_sequence_ = sequence_;
        durable_offset_ = end_offset_;
        durable_crc_ = last_crc_;
    } catch (...) {
        (void)::unlink(temporary.c_str()); // only our exact, uniquely created temp file
        throw;
    }
}

void AofLog::synchronize(int fd) {
    int result;
    do {
        result = io_.sync ? io_.sync(fd) : ::fsync(fd);
    } while (result < 0 && errno == EINTR);
    if (result != 0) {
        io_error("fsync");
    }
}

void AofLog::require_healthy() const {
    if (failed_) {
        invalid("persistence failed; restart and inspect journal before serving");
    }
}

void AofLog::recover(storage::Database& database) {
    require_healthy();
    try {
        if (recovered_ || database.size() != 0) {
            invalid("replay requires a fresh database and journal handle");
        }
        struct stat info{};
        if (::fstat(fd_.get(), &info) != 0) {
            io_error("stat before replay");
        }
        if (info.st_size < static_cast<off_t>(FileHeaderBytes) ||
            durable_offset_ > static_cast<std::uint64_t>(info.st_size)) {
            invalid("journal shortened before replay; files left unchanged");
        }
        off_t position = FileHeaderBytes;
        while (position < info.st_size) {
            const auto remaining = info.st_size - position;
            if (remaining < static_cast<off_t>(RecordHeaderBytes)) {
                invalid("truncated record header; file left unchanged");
            }
            auto record = read_at(fd_.get(), RecordHeaderBytes, position);
            Reader header(std::string_view(record).substr(4));
            const auto sequence = header.number(8);
            const auto length = header.number(4);
            if (record.substr(0, 4) != "TXN1" || sequence_ == UINT64_MAX ||
                sequence != sequence_ + 1 || length > MaxAofRecordBytes) {
                invalid("invalid record magic, sequence or length");
            }
            const auto total = RecordHeaderBytes + length + 8;
            if (total > static_cast<std::uint64_t>(remaining)) {
                invalid("truncated transaction tail; file left unchanged");
            }
            record += read_at(fd_.get(), static_cast<std::size_t>(length) + 8,
                              position + static_cast<off_t>(RecordHeaderBytes));
            const auto payload_end = RecordHeaderBytes + static_cast<std::size_t>(length);
            Reader footer(std::string_view(record).substr(payload_end));
            const auto record_crc = static_cast<std::uint32_t>(footer.number(4));
            if (record_crc != crc32c(std::string_view(record).substr(0, payload_end)) ||
                record.substr(payload_end + 4) != "END!") {
                invalid("record checksum or commit trailer mismatch; file left unchanged");
            }
            database.replay_effects(decode_effects(std::string_view(record).substr(
                RecordHeaderBytes, static_cast<std::size_t>(length))));
            sequence_ = sequence;
            position += static_cast<off_t>(total);
            last_crc_ = record_crc;
            if (sequence_ == durable_sequence_ &&
                (static_cast<std::uint64_t>(position) != durable_offset_ ||
                 last_crc_ != durable_crc_)) {
                invalid("durable record disagrees with commit watermark");
            }
        }
        if (sequence_ < durable_sequence_) {
            invalid("committed records are missing");
        }
        end_offset_ = static_cast<std::uint64_t>(position);
        // Startup expiry is a real state transition. Journal its tombstones
        // before future LRU records can omit those keys from their key order.
        storage::Database::Transaction cleanup(database);
        database.finish_recovery();
        const auto cleanup_effects = cleanup.effects();
        if (::lseek(fd_.get(), 0, SEEK_END) < 0) {
            io_error("seek append position");
        }
        synchronize(fd_.get()); // establish a checked durable recovered prefix before readiness
        publish_watermark();
        recovered_ = true;
        append(cleanup_effects);
        if (pending_) {
            flush(); // even weaker policies start with a synchronized recovered baseline
        }
        cleanup.commit();
    } catch (...) {
        failed_ = true;
        throw;
    }
}

void AofLog::append(const storage::CommittedEffects& effects) {
    require_healthy();
    try {
        if (!recovered_) {
            invalid("append before successful replay");
        }
        if (effects.entries.empty()) {
            return;
        }
        if (sequence_ == UINT64_MAX) {
            invalid("sequence exhausted");
        }
        const auto payload = encode_effects(effects);
        Writer header;
        header.bytes = "TXN1";
        header.number(sequence_ + 1, 8);
        header.number(payload.size(), 4);
        std::string frame = std::move(header.bytes);
        frame += payload;
        Writer trailer;
        const auto checksum = crc32c(frame);
        trailer.number(checksum, 4);
        frame += trailer.bytes;
        frame += "END!";
        pending_ = true;
        if (frame.size() >
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - end_offset_) {
            invalid("journal file offset exhausted");
        }
        write_all(frame);
        ++sequence_;
        end_offset_ += frame.size();
        last_crc_ = checksum;
        if (policy_ == SyncPolicy::Always) {
            flush();
        }
    } catch (...) {
        failed_ = true;
        throw;
    }
}

void AofLog::flush() {
    synchronize(fd_.get());
    publish_watermark();
    pending_ = false;
    next_sync_ = clock_.monotonic_now() + std::chrono::seconds{1};
}

void AofLog::tick() {
    require_healthy();
    try {
        if (pending_ && policy_ == SyncPolicy::EverySecond &&
            clock_.monotonic_now() >= next_sync_) {
            flush();
        }
    } catch (...) {
        failed_ = true;
        throw;
    }
}

void AofLog::finish() {
    require_healthy();
    try {
        if (pending_) {
            flush();
        }
    } catch (...) {
        failed_ = true;
        throw;
    }
}
} // namespace redisdb::persistence
