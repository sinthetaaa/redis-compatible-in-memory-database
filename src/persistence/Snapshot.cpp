#include "redisdb/persistence/Snapshot.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace redisdb::persistence {
namespace {
constexpr std::size_t HeaderBytes = 80;
constexpr std::size_t MaxKeyBytes = 8 * 1024 * 1024;

[[noreturn]] void invalid(const char* message) {
    throw PersistenceError(std::string("snapshot ") + message);
}
[[noreturn]] void io_error(const char* operation) {
    throw PersistenceError(std::string("snapshot ") + operation + ": " +
                           std::error_code(errno, std::generic_category()).message());
}
void put_number(std::string& bytes, std::uint64_t value, unsigned width) {
    for (unsigned i = width; i != 0; --i) {
        bytes.push_back(static_cast<char>((value >> ((i - 1) * 8)) & 255U));
    }
}
std::uint64_t get_number(std::string_view bytes, std::size_t pos, unsigned width) {
    if (pos > bytes.size() || width > bytes.size() - pos) {
        invalid("truncated integer");
    }
    std::uint64_t result = 0;
    for (unsigned i = 0; i < width; ++i) {
        result = (result << 8) | static_cast<unsigned char>(bytes[pos + i]);
    }
    return result;
}
net::UniqueFd open_regular(const std::filesystem::path& path) {
    net::UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat info{};
    if (!fd || ::fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode)) {
        invalid("missing, symlink or nonregular recovery file");
    }
    return fd;
}
std::string read_at(int fd, std::size_t count, std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        count > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - offset) {
        invalid("file offset overflow");
    }
    std::string result(count, '\0');
    std::size_t done = 0;
    while (done < count) {
        const auto n =
            ::pread(fd, result.data() + done, count - done, static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            io_error("read");
        }
        if (n == 0) {
            invalid("truncated recovery file");
        }
        done += static_cast<std::size_t>(n);
    }
    return result;
}
std::uint64_t file_size(int fd) {
    struct stat info{};
    if (::fstat(fd, &info) != 0 || info.st_size < 0) {
        io_error("stat");
    }
    return static_cast<std::uint64_t>(info.st_size);
}
// Checksummed frames are bounded individually, not by total database size.
std::string frame(std::string_view tag, std::string_view payload) {
    if (payload.size() > MaxAofRecordBytes) {
        invalid("oversized frame");
    }
    std::string result(tag);
    put_number(result, payload.size(), 4);
    result += payload;
    put_number(result, crc32c(result), 4);
    result += "END!";
    return result;
}
struct Temporary {
    std::string path;
    net::UniqueFd fd;
    explicit Temporary(const std::filesystem::path& base) : path(base.string() + ".tmp.XXXXXX") {
        fd.reset(::mkstemp(path.data()));
        if (!fd) {
            io_error("create temporary file");
        }
        if (::fcntl(fd.get(), F_SETFD, FD_CLOEXEC) < 0) {
            (void)::unlink(path.c_str());
            io_error("set close-on-exec");
        }
    }
    ~Temporary() { (void)::unlink(path.c_str()); }
};
bool path_exists(const std::filesystem::path& path) {
    struct stat info{};
    if (::lstat(path.c_str(), &info) == 0) {
        return true;
    }
    if (errno != ENOENT) {
        io_error("inspect path");
    }
    return false;
}
} // namespace

SnapshotStore::SnapshotStore(const SnapshotOptions& options, SnapshotIo io)
    : path_(options.directory), io_(std::move(io)) {
    if (path_.empty()) {
        invalid("storage directory is empty");
    }
    // POSIX resolves a trailing slash or dot before applying O_NOFOLLOW.
    // Remove only these final components; preserve '..' and trusted ancestors.
    while (path_.has_relative_path() && path_ != "." &&
           (path_.filename().empty() || path_.filename() == ".")) {
        const auto parent = path_.parent_path();
        path_ = parent.empty() ? std::filesystem::path{"."} : parent;
    }
    directory_.reset(::open(path_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!directory_) {
        io_error("open storage directory");
    }
    lock_.reset(::open((path_ / "LOCK").c_str(),
                       O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
    struct stat info{};
    if (!lock_ || ::fstat(lock_.get(), &info) != 0 || !S_ISREG(info.st_mode)) {
        invalid("invalid directory lock file");
    }
    if (::flock(lock_.get(), LOCK_EX | LOCK_NB) != 0) {
        io_error("exclusive directory lock");
    }
    const auto current = path_ / "CURRENT";
    if (!path_exists(current)) {
        // First initialization is distinguishable from a lost selector: final
        // generations without CURRENT are inconsistent, never an AOF fallback.
        for (const auto& entry : std::filesystem::directory_iterator(path_)) {
            if (entry.path().filename().string().starts_with("generation-")) {
                invalid("generation exists without CURRENT");
            }
        }
        select(0); // durable empty selection before constructing any generation
    } else {
        auto fd = open_regular(current);
        if (file_size(fd.get()) != 32) {
            invalid("invalid CURRENT size");
        }
        const auto bytes = read_at(fd.get(), 32, 0);
        if (bytes.substr(0, 8) != "RDBSEL01" || get_number(bytes, 8, 4) != 1 ||
            get_number(bytes, 12, 4) != 0 ||
            get_number(bytes, 24, 4) != crc32c(bytes.substr(0, 24)) || bytes.substr(28) != "END!") {
            invalid("invalid CURRENT identification, version or checksum");
        }
        generation_ = get_number(bytes, 16, 8);
    }
}

std::filesystem::path SnapshotStore::data_path(std::uint64_t generation) const {
    return path_ / ("generation-" + std::to_string(generation) + ".snap");
}
void SnapshotStore::write_all(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto n = io_.file.write ? io_.file.write(fd, bytes.data(), bytes.size())
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
void SnapshotStore::sync(int fd) {
    int result;
    do {
        result = io_.file.sync ? io_.file.sync(fd) : ::fsync(fd);
    } while (result < 0 && errno == EINTR);
    if (result != 0) {
        io_error("fsync");
    }
}
void SnapshotStore::rename(const std::filesystem::path& from, const std::filesystem::path& to) {
    const int result =
        io_.rename ? io_.rename(from.c_str(), to.c_str()) : ::rename(from.c_str(), to.c_str());
    if (result != 0) {
        io_error("rename");
    }
}
void SnapshotStore::boundary(SnapshotBoundary point) {
    if (io_.boundary) {
        io_.boundary(point);
    }
}
void SnapshotStore::select(std::uint64_t generation) {
    std::string bytes = "RDBSEL01";
    put_number(bytes, 1, 4);
    put_number(bytes, 0, 4);
    put_number(bytes, generation, 8);
    put_number(bytes, crc32c(bytes), 4);
    bytes += "END!";
    Temporary temporary(path_ / "CURRENT");
    write_all(temporary.fd.get(), bytes);
    boundary(SnapshotBoundary::ManifestWritten);
    sync(temporary.fd.get());
    boundary(SnapshotBoundary::ManifestSynced);
    rename(temporary.path, path_ / "CURRENT");
    boundary(SnapshotBoundary::ManifestRenamed);
    sync(directory_.get());
    generation_ = generation;
    boundary(SnapshotBoundary::Published);
}

std::uint64_t SnapshotStore::publish(const storage::Database& database, AofLog& journal) {
    if (!restored_ || failed_) {
        invalid("publication requires successful recovery and healthy store");
    }
    try {
        const auto checkpoint = journal.checkpoint(); // includes weaker-policy writes
        auto candidate = generation_;
        do {
            if (candidate == UINT64_MAX) {
                invalid("generation exhausted");
            }
            ++candidate;
        } while (path_exists(data_path(candidate)));
        // Retain interrupted candidates; never overwrite an existing generation.
        const auto order = database.snapshot_order();
        std::string header = "RDBSNP01";
        put_number(header, 1, 4);
        put_number(header, order.has_lru ? 1 : 0, 4);
        put_number(header, candidate, 8);
        put_number(header, checkpoint.sequence, 8);
        put_number(header, checkpoint.offset, 8);
        put_number(header, checkpoint.record_crc, 4);
        put_number(header, checkpoint.prefix_crc, 4);
        put_number(header, database.size(), 8);
        put_number(header, database.accounted_bytes(), 8);
        put_number(header, order.lru_order.size(), 8);
        put_number(header, 1, 4); // logical accounting model version
        put_number(header, crc32c(header), 4);
        Temporary temporary(path_ / "snapshot");
        std::uint32_t checksum = 0;
        auto emit = [&](std::string_view bytes) {
            write_all(temporary.fd.get(), bytes);
            checksum = extend_crc32c(checksum, bytes);
        };
        emit(header);
        database.visit_snapshot([&](const std::string& key, const storage::Entry& entry) {
            storage::CommittedEffects effect;
            effect.entries.push_back({key, entry});
            emit(frame("KEY1", encode_effects(effect)));
        });
        for (const auto& key : order.lru_order) {
            emit(frame("LRU1", key));
        }
        std::string footer;
        put_number(footer, checksum, 4);
        write_all(temporary.fd.get(), frame("DONE", footer));
        boundary(SnapshotBoundary::DataWritten);
        sync(temporary.fd.get());
        boundary(SnapshotBoundary::DataSynced);
        rename(temporary.path, data_path(candidate));
        boundary(SnapshotBoundary::DataRenamed);
        sync(directory_.get());
        boundary(SnapshotBoundary::DataDirectorySynced);
        select(candidate);
        return generation_;
    } catch (...) {
        failed_ = true; // uncertain final rename/sync: stop serving, recover on restart
        throw;
    }
}

std::optional<AofCheckpoint> SnapshotStore::restore(storage::Database& database) {
    if (restored_ || database.size() != 0 || failed_) {
        invalid("restore requires fresh database and store");
    }
    try {
        if (generation_ == 0) {
            restored_ = true;
            return std::nullopt;
        }
        auto fd = open_regular(data_path(generation_));
        const auto size = file_size(fd.get());
        const auto header = read_at(fd.get(), HeaderBytes, 0);
        const auto flags = get_number(header, 12, 4);
        if (header.substr(0, 8) != "RDBSNP01" || get_number(header, 8, 4) != 1 || flags > 1 ||
            get_number(header, 16, 8) != generation_ || get_number(header, 72, 4) != 1 ||
            get_number(header, 76, 4) != crc32c(header.substr(0, 76))) {
            invalid("invalid header magic, version, generation, accounting model or checksum");
        }
        AofCheckpoint checkpoint{get_number(header, 24, 8), get_number(header, 32, 8),
                                 static_cast<std::uint32_t>(get_number(header, 40, 4)),
                                 static_cast<std::uint32_t>(get_number(header, 44, 4))};
        const auto count = get_number(header, 48, 8);
        const auto charge = get_number(header, 56, 8);
        const auto lru_count = get_number(header, 64, 8);
        // Counts are file-size bounded before allocation or iteration. There is
        // no aggregate 64 MiB/1M-entry snapshot ceiling.
        if (size < HeaderBytes + 20 || count > (size - HeaderBytes - 20) / 16 ||
            lru_count > (size - HeaderBytes - 20) / 16 || (flags == 0 && lru_count != 0) ||
            (flags == 1 && lru_count != count)) {
            invalid("invalid entry/LRU counts");
        }
        std::uint64_t position = HeaderBytes;
        std::uint32_t checksum = crc32c(header);
        auto consume = [&](std::string_view tag, std::size_t limit) {
            if (position > size || size - position < 16) {
                invalid("truncated frame");
            }
            auto bytes = read_at(fd.get(), 8, position);
            const auto length = get_number(bytes, 4, 4);
            if (bytes.substr(0, 4) != tag || length > limit || length > size - position - 16) {
                invalid("invalid frame tag or length");
            }
            bytes += read_at(fd.get(), static_cast<std::size_t>(length) + 8, position + 8);
            if (get_number(bytes, 8 + static_cast<std::size_t>(length), 4) !=
                    crc32c(
                        std::string_view(bytes).substr(0, 8 + static_cast<std::size_t>(length))) ||
                bytes.substr(bytes.size() - 4) != "END!") {
                invalid("frame checksum or trailer mismatch");
            }
            position += bytes.size();
            checksum = extend_crc32c(checksum, bytes);
            return bytes.substr(8, static_cast<std::size_t>(length));
        };
        std::optional<std::string> previous;
        for (std::uint64_t i = 0; i < count; ++i) {
            auto effects = decode_effects(consume("KEY1", MaxAofRecordBytes));
            if (effects.entries.size() != 1 || !effects.entries.front().replacement ||
                effects.has_lru || !effects.lru_order.empty() ||
                (previous && effects.entries.front().key <= *previous)) {
                invalid("invalid snapshot key order or entry");
            }
            previous = effects.entries.front().key;
            database.restore_snapshot_entry(std::move(effects.entries.front()));
        }
        storage::CommittedEffects order;
        order.has_lru = flags == 1;
        for (std::uint64_t i = 0; i < lru_count; ++i) {
            order.lru_order.push_back(consume("LRU1", MaxKeyBytes));
        }
        // Validate the persisted order even when current policy is noeviction.
        if (order.has_lru) {
            std::set<std::string_view> unique;
            database.visit_snapshot(
                [&](const std::string& key, const storage::Entry&) { unique.insert(key); });
            for (const auto& key : order.lru_order) {
                if (unique.erase(key) != 1) {
                    invalid("invalid LRU key");
                }
            }
            if (!unique.empty()) {
                invalid("incomplete LRU key order");
            }
        }
        const auto expected_crc = checksum;
        const auto footer = consume("DONE", 4);
        if (footer.size() != 4 || get_number(footer, 0, 4) != expected_crc || position != size ||
            database.size() != count || database.accounted_bytes() != charge) {
            invalid("footer checksum, trailing bytes or logical memory charge mismatch");
        }
        database.replay_effects(order); // reconstruct current-policy LRU/index iterators
        restored_ = true;
        return checkpoint;
    } catch (...) {
        failed_ = true;
        throw;
    }
}
} // namespace redisdb::persistence
