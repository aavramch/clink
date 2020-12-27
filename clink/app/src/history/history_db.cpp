// Copyright (c) 2017 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "history_db.h"
#include "utils/app_context.h"

#include <core/base.h>
#include <core/globber.h>
#include <core/os.h>
#include <core/settings.h>
#include <core/str.h>
#include <core/str_tokeniser.h>
#include <core/path.h>
#include <core/log.h>
#include <assert.h>

#include <new>
#include <Windows.h>
extern "C" {
#include <readline/history.h>
}

//------------------------------------------------------------------------------
static setting_bool g_shared(
    "history.shared",
    "Share history between instances",
    "",
    false);

namespace use_get_max_history_instead {
static setting_int g_max_history(
    "history.max_lines",
    "The number of history lines to save",
    "The number of history lines to save if history.save is enabled (1 to 50000).",
    2500);
};

static setting_bool g_ignore_space(
    "history.ignore_space",
    "Skip adding lines prefixed with whitespace",
    "Ignore lines that begin with whitespace when adding lines in to\n"
    "the history.",
    true);

static setting_enum g_dupe_mode(
    "history.dupe_mode",
    "Controls how duplicate entries are handled",
    "If a line is a duplicate of an existing history entry Clink will erase\n"
    "the duplicate when this is set to 'erase_prev'. A value of 'ignore' will\n"
    "not add a line to the history if it already exists, and a value of 'add'\n"
    "will always add lines.\n"
    "Note that history is not deduplicated when reading/writing to disk.",
    "add,ignore,erase_prev",
    2);

static setting_enum g_expand_mode(
    "history.expand_mode",
    "Sets how command history expansion is applied",
    "The '!' character in an entered line can be interpreted to introduce\n"
    "words from the history. That can be enabled and disable by setting this\n"
    "value to 'on' or 'off'. Or set this to 'not_squoted', 'not_dquoted', or\n"
    "'not_quoted' to skip any '!' character in single, double, or both quotes\n"
    "respectively.",
    "off,on,not_squoted,not_dquoted,not_quoted",
    4);

static size_t get_max_history()
{
    size_t limit = use_get_max_history_instead::g_max_history.get();
    if (!limit || limit > 50000)
        limit = 50000;
    return limit;
}



//------------------------------------------------------------------------------
static int history_expand_control(char* line, int marker_pos)
{
    int setting, in_quote, i;

    setting = g_expand_mode.get();
    if (setting <= 1)
        return (setting <= 0);

    // Is marker_pos inside a quote of some kind?
    in_quote = 0;
    for (i = 0; i < marker_pos && *line; ++i, ++line)
    {
        int c = *line;
        if (c == '\'' || c == '\"')
            in_quote = (c == in_quote) ? 0 : c;
    }

    switch (setting)
    {
    case 2: return (in_quote == '\'');
    case 3: return (in_quote == '\"');
    case 4: return (in_quote == '\"' || in_quote == '\'');
    }

    return 0;
}

//------------------------------------------------------------------------------
static void get_file_path(str_base& out, bool session)
{
    out.clear();

    const auto* app = app_context::get();
    app->get_history_path(out);

    if (session)
    {
        str<16> suffix;
        suffix.format("_%d", app->get_id());
        out << suffix;
    }
}

//------------------------------------------------------------------------------
static void* open_file(const char* path)
{
    DWORD share_flags = FILE_SHARE_READ|FILE_SHARE_WRITE;
    void* handle = CreateFile(path, GENERIC_READ|GENERIC_WRITE, share_flags,
        nullptr, OPEN_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

    return (handle == INVALID_HANDLE_VALUE) ? nullptr : handle;
}



//------------------------------------------------------------------------------
const int max_ctag_size = 6 + 10 + 1 + 10 + 1 + 10 + 1 + 10 + 1 + 1;
void concurrency_tag::generate_new_tag()
{
    static unsigned int disambiguate = 0;

    assert(m_tag.empty());
    time_t now = time(nullptr);
    m_tag.format("|CTAG_%u_%u_%u_%u", now, GetTickCount(), GetProcessId(GetCurrentProcess()), disambiguate++);
}

//------------------------------------------------------------------------------
void concurrency_tag::set(const char* tag)
{
    assert(m_tag.empty());
    m_tag = tag;
}



//------------------------------------------------------------------------------
class auto_free_str
{
public:
                        auto_free_str() = default;
                        auto_free_str(const char* s, int len) { set(s, len); }
                        auto_free_str(auto_free_str&& other) : m_ptr(other.m_ptr) { other.m_ptr = nullptr; }
                        ~auto_free_str() { free(m_ptr); }

    auto_free_str&      operator=(const char* s) { set(s); return *this; }
    auto_free_str&      operator=(auto_free_str&& other) { m_ptr = other.m_ptr; other.m_ptr = nullptr; return *this; }
    void                set(const char* s, int len = -1);
    const char*         get() const { return m_ptr; }

private:
    char*               m_ptr = nullptr;
};

//------------------------------------------------------------------------------
void auto_free_str::set(const char* s, int len)
{
    if (s == m_ptr)
    {
        if (len < int(strlen(m_ptr)))
            m_ptr[len] = '\0';
    }
    else
    {
        char* old = m_ptr;
        if (len < 0)
            len = int(strlen(s));
        m_ptr = (char*)malloc(len + 1);
        memcpy(m_ptr, s, len);
        m_ptr[len] = '\0';
        free(old);
    }
}



//------------------------------------------------------------------------------
union line_id_impl
{
    explicit            line_id_impl()                  { outer = 0; }
    explicit            line_id_impl(unsigned int o)    { offset = o; bank_index = 0; active = 1; }
    explicit            operator bool () const          { return !!outer; }
    struct {
        unsigned int    offset : 29;
        unsigned int    bank_index : 2;
        unsigned int    active : 1;
    };
    history_db::line_id outer;
};



//------------------------------------------------------------------------------
class bank_lock
    : public no_copy
{
public:
    explicit        operator bool () const;

protected:
                    bank_lock() = default;
                    bank_lock(void* handle, bool exclusive);
                    ~bank_lock();
    void*           m_handle = nullptr;
};

//------------------------------------------------------------------------------
bank_lock::bank_lock(void* handle, bool exclusive)
: m_handle(handle)
{
    if (m_handle == nullptr)
        return;

    OVERLAPPED overlapped = {};
    int flags = exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    LockFileEx(m_handle, flags, 0, ~0u, ~0u, &overlapped);
}

//------------------------------------------------------------------------------
bank_lock::~bank_lock()
{
    if (m_handle != nullptr)
    {
        OVERLAPPED overlapped = {};
        UnlockFileEx(m_handle, 0, ~0u, ~0u, &overlapped);
    }
}

//------------------------------------------------------------------------------
bank_lock::operator bool () const
{
    return (m_handle != nullptr);
}



//------------------------------------------------------------------------------
class read_lock
    : public bank_lock
{
public:
    class file_iter : public no_copy
    {
    public:
                            file_iter() = default;
                            file_iter(const read_lock& lock, char* buffer, int buffer_size);
        template <int S>    file_iter(const read_lock& lock, char (&buffer)[S]);
        unsigned int        next(unsigned int rollback=0);
        unsigned int        get_buffer_offset() const   { return m_buffer_offset; }
        char*               get_buffer() const          { return m_buffer; }
        unsigned int        get_buffer_size() const     { return m_buffer_size; }
        unsigned int        get_remaining() const       { return m_remaining; }
        void                set_file_offset(unsigned int offset);

    private:
        char*               m_buffer;
        void*               m_handle;
        unsigned int        m_buffer_size;
        unsigned int        m_buffer_offset;
        unsigned int        m_remaining;
    };

    class line_iter : public no_copy
    {
    public:
                            line_iter() = default;
                            line_iter(const read_lock& lock, char* buffer, int buffer_size);
        template <int S>    line_iter(const read_lock& lock, char (&buffer)[S]);
        line_id_impl        next(str_iter& out);
        void                set_file_offset(unsigned int offset);
        unsigned int        get_deleted_count() const { return m_deleted; }

    private:
        bool                provision();
        file_iter           m_file_iter;
        unsigned int        m_remaining = 0;
        unsigned int        m_deleted = 0;
        bool                m_first_line = true;
        bool                m_eating_ctag = false;
    };

    explicit                read_lock() = default;
    explicit                read_lock(void* handle, bool exclusive=false);
    line_id_impl            find(const char* line) const;
    template <class T> void find(const char* line, T&& callback) const;
};

//------------------------------------------------------------------------------
read_lock::read_lock(void* handle, bool exclusive)
: bank_lock(handle, exclusive)
{
}

//------------------------------------------------------------------------------
template <class T> void read_lock::find(const char* line, T&& callback) const
{
    char buffer[history_db::max_line_length];
    line_iter iter(*this, buffer);

    line_id_impl id;
    for (str_iter read; id = iter.next(read);)
    {
        if (strncmp(line, read.get_pointer(), read.length()) != 0)
            continue;

        if (line[read.length()] != '\0')
            continue;

        unsigned int file_ptr = SetFilePointer(m_handle, 0, nullptr, FILE_CURRENT);
        bool more = callback(id);
        SetFilePointer(m_handle, file_ptr, nullptr, FILE_BEGIN);

        if (!more)
            break;
    }
}

//------------------------------------------------------------------------------
line_id_impl read_lock::find(const char* line) const
{
    line_id_impl id;
    find(line, [&] (line_id_impl inner_id) {
        id = inner_id;
        return false;
    });
    return id;
}



//------------------------------------------------------------------------------
template <int S> read_lock::file_iter::file_iter(const read_lock& lock, char (&buffer)[S])
: file_iter(lock, buffer, S)
{
}

//------------------------------------------------------------------------------
read_lock::file_iter::file_iter(const read_lock& lock, char* buffer, int buffer_size)
: m_handle(lock.m_handle)
, m_buffer(buffer)
, m_buffer_size(buffer_size)
{
    set_file_offset(0);
}

//------------------------------------------------------------------------------
unsigned int read_lock::file_iter::next(unsigned int rollback)
{
    if (!m_remaining)
        return (m_buffer[0] = '\0');

    rollback = min<unsigned>(rollback, m_buffer_size);
    if (rollback)
        memmove(m_buffer, m_buffer + m_buffer_size - rollback, rollback);

    m_buffer_offset += m_buffer_size - rollback;

    char* target = m_buffer + rollback;
    int needed = min(m_remaining, m_buffer_size - rollback);

    DWORD read = 0;
    ReadFile(m_handle, target, needed, &read, nullptr);

    m_remaining -= read;
    m_buffer_size = read + rollback;
    return m_buffer_size;
}

//------------------------------------------------------------------------------
void read_lock::file_iter::set_file_offset(unsigned int offset)
{
    m_remaining = GetFileSize(m_handle, nullptr);
    offset = clamp(offset, (unsigned int)0, m_remaining);
    m_remaining -= offset;
    m_buffer_offset = 0 - m_buffer_size;
    SetFilePointer(m_handle, offset, nullptr, FILE_BEGIN);
    m_buffer[0] = '\0';
}



//------------------------------------------------------------------------------
template <int S> read_lock::line_iter::line_iter(const read_lock& lock, char (&buffer)[S])
: line_iter(lock, buffer, S)
{
}

//------------------------------------------------------------------------------
read_lock::line_iter::line_iter(const read_lock& lock, char* buffer, int buffer_size)
: m_file_iter(lock, buffer, buffer_size)
{
}

//------------------------------------------------------------------------------
bool read_lock::line_iter::provision()
{
    return !!(m_remaining = m_file_iter.next(m_remaining));
}

//------------------------------------------------------------------------------
inline bool is_line_breaker(unsigned char c)
{
    return c == 0x00 || c == 0x0a || c == 0x0d;
}

//------------------------------------------------------------------------------
line_id_impl read_lock::line_iter::next(str_iter& out)
{
    while (m_remaining || provision())
    {
        const char* last = m_file_iter.get_buffer() + m_file_iter.get_buffer_size();
        const char* start = last - m_remaining;

        bool first_line = m_first_line || m_eating_ctag;
        bool eating_ctag = m_eating_ctag;

        for (; start != last; ++start, --m_remaining)
            if (!is_line_breaker(*start))
            {
                if (m_first_line)
                {
                    if (*start == '|')
                    {
                        // The <6 is a concession for the history tests.  They
                        // can read with a buffer smaller than 6 characters, but
                        // they don't really understand the CTAG and need the
                        // CTAG line completely hidden from their view, even if
                        // they're using pathologically small buffers.
                        bool eat = (last - start < 6 || strncmp(start, "|CTAG_", 6) == 0);
                        m_eating_ctag = eating_ctag = eat;
                    }
                    m_first_line = false;
                }
                break;
            }

        const char* end = start;
        for (; end != last; ++end)
            if (is_line_breaker(*end))
            {
                m_eating_ctag = false;
                break;
            }

        if (end == last && start != m_file_iter.get_buffer())
        {
            provision();
            continue;
        }

        int bytes = int(end - start);
        m_remaining -= bytes;

        bool was_first_line = m_first_line;
        m_first_line = false;

        if (*start == '|' || eating_ctag)
        {
            if (!eating_ctag)
                ++m_deleted;
            continue;
        }

        new (&out) str_iter(start, int(end - start));

        unsigned int offset = int(start - m_file_iter.get_buffer());
        return line_id_impl(m_file_iter.get_buffer_offset() + offset);
    }

    return line_id_impl();
}

//------------------------------------------------------------------------------
void read_lock::line_iter::set_file_offset(unsigned int offset)
{
    m_file_iter.set_file_offset(offset);
    m_eating_ctag = false;
}



//------------------------------------------------------------------------------
class write_lock
    : public read_lock
{
public:
                    write_lock() = default;
    explicit        write_lock(void* handle);
    void            clear();
    void            add(const char* line);
    void            remove(line_id_impl id);
    void            append(const read_lock& src);
};

//------------------------------------------------------------------------------
write_lock::write_lock(void* handle)
: read_lock(handle, true)
{
}

//------------------------------------------------------------------------------
void write_lock::clear()
{
    SetFilePointer(m_handle, 0, nullptr, FILE_BEGIN);
    SetEndOfFile(m_handle);
}

//------------------------------------------------------------------------------
void write_lock::add(const char* line)
{
    DWORD written;
    SetFilePointer(m_handle, 0, nullptr, FILE_END);
    WriteFile(m_handle, line, int(strlen(line)), &written, nullptr);
    WriteFile(m_handle, "\n", 1, &written, nullptr);
}

//------------------------------------------------------------------------------
void write_lock::remove(line_id_impl id)
{
    DWORD written;
    SetFilePointer(m_handle, id.offset, nullptr, FILE_BEGIN);
    WriteFile(m_handle, "|", 1, &written, nullptr);
}

//------------------------------------------------------------------------------
void write_lock::append(const read_lock& src)
{
    DWORD written;

    SetFilePointer(m_handle, 0, nullptr, FILE_END);

    char buffer[history_db::max_line_length];
    read_lock::file_iter src_iter(src, buffer);
    while (int bytes_read = src_iter.next())
        WriteFile(m_handle, buffer, bytes_read, &written, nullptr);
}



//------------------------------------------------------------------------------
class read_line_iter
{
public:
                            read_line_iter(const history_db& db, unsigned int this_size);
    history_db::line_id     next(str_iter& out);

private:
    bool                    next_bank();
    const history_db&       m_db;
    read_lock               m_lock;
    read_lock::line_iter    m_line_iter;
    unsigned int            m_buffer_size;
    unsigned int            m_bank_index = 0;
};

//------------------------------------------------------------------------------
read_line_iter::read_line_iter(const history_db& db, unsigned int this_size)
: m_db(db)
, m_buffer_size(this_size - sizeof(*this))
{
    next_bank();
}

//------------------------------------------------------------------------------
bool read_line_iter::next_bank()
{
    while (m_bank_index < sizeof_array(m_db.m_bank_handles))
    {
        unsigned int bank_index = m_bank_index++;
        if (void* bank_handle = m_db.m_bank_handles[bank_index])
        {
            char* buffer = (char*)(this + 1);
            m_lock.~read_lock();
            new (&m_lock) read_lock(bank_handle);
            new (&m_line_iter) read_lock::line_iter(m_lock, buffer, m_buffer_size);
            return true;
        }
    }

    return false;
}

//------------------------------------------------------------------------------
history_db::line_id read_line_iter::next(str_iter& out)
{
    if (m_bank_index > sizeof_array(m_db.m_bank_handles))
        return 0;

    do
    {
        if (line_id_impl ret = m_line_iter.next(out))
        {
            ret.bank_index = m_bank_index - 1;
            return ret.outer;
        }
    }
    while (next_bank());

    return 0;
}



//------------------------------------------------------------------------------
history_db::iter::~iter()
{
    if (impl)
        ((read_line_iter*)impl)->~read_line_iter();
}

//------------------------------------------------------------------------------
history_db::line_id history_db::iter::next(str_iter& out)
{
    return impl ? ((read_line_iter*)impl)->next(out) : 0;
}



//------------------------------------------------------------------------------
static bool extract_ctag(const read_lock& lock, concurrency_tag& tag)
{
    char buffer[max_ctag_size];
    read_lock::file_iter iter(lock, buffer);

    int bytes_read = iter.next();
    if (bytes_read <= 0)
    {
        LOG("read %d bytes", bytes_read);
        return false;
    }

    if (bytes_read >= sizeof(buffer))
        bytes_read = sizeof(buffer) - 1;
    buffer[bytes_read] = 0;

    if (strncmp(buffer, "|CTAG_", 6) != 0)
    {
        LOG("first line not a ctag");
        return false;
    }

    char* eol = strpbrk(buffer, "\r\n");
    if (!eol)
    {
        LOG("first line has no line ending");
        return false;
    }
    *eol = '\0';

    tag.set(buffer);
    return true;
}

//------------------------------------------------------------------------------
static void rewrite_master_bank(write_lock& lock, int max_line_length)
{
    max_line_length++;
    char* buffer = (char*)malloc(max_line_length);

    // Read lines to keep into vector.
    str_iter out;
    read_lock::line_iter iter(lock, buffer, max_line_length);
    std::vector<auto_free_str> lines_to_keep;
    while (iter.next(out))
        lines_to_keep.push_back(std::move(auto_free_str(out.get_pointer(), out.length())));

    // Clear and write new tag.
    concurrency_tag tag;
    tag.generate_new_tag();
    lock.clear();
    lock.add(tag.get());

    // Write lines from vector.
    for (auto const& line : lines_to_keep)
        lock.add(line.get());

    free(buffer);
}

//------------------------------------------------------------------------------
static void migrate_history(const char* path)
{
    void* bank_handle = open_file(path);
    if (!bank_handle)
        return;

    // First lock the history bank.
    write_lock lock(bank_handle);

    // Then test if it's empty -- only migrate if it's empty.
    DWORD high = 0;
    DWORD low = GetFileSize(bank_handle, &high);
    if (!low && !high)
    {
        // Build the old history file name.
        str<> old_file;
        path::get_directory(path, old_file);
        path::append(old_file, ".history");

        // Open the old history file and try to migrate.
        FILE* old = fopen(old_file.c_str(), "r");
        if (old)
        {
            // Clear and write new tag.
            concurrency_tag tag;
            tag.generate_new_tag();
            lock.clear();
            lock.add(tag.get());

            // Copy old history.
            int buffer_size = 8192;
            char* buffer = static_cast<char*>(malloc(buffer_size));
            while (fgets(buffer, buffer_size, old))
            {
                size_t len = strlen(buffer);
                while (len--)
                {
                    char c = buffer[len];
                    if (c != '\n' && c != '\r')
                        break;
                    buffer[len] = '\0';
                }
                lock.add(buffer);
            }

            fclose(old);
        }
    }

    CloseHandle(bank_handle);
}



//------------------------------------------------------------------------------
history_db::history_db()
{
    memset(m_bank_handles, 0, sizeof(m_bank_handles));
    m_master_len = 0;
    m_master_deleted_count = 0;

    // Create a self-deleting file to used to indicate this session's alive
    str<280> path;
    get_file_path(path, true);
    path << "~";

    DWORD flags = FILE_FLAG_DELETE_ON_CLOSE|FILE_ATTRIBUTE_HIDDEN;
    m_alive_file = CreateFile(path.c_str(), 0, 0, nullptr, CREATE_ALWAYS, flags, nullptr);
    m_alive_file = (m_alive_file == INVALID_HANDLE_VALUE) ? nullptr : m_alive_file;

    history_inhibit_expansion_function = history_expand_control;

    static_assert(sizeof(line_id) == sizeof(line_id_impl), "");
}

//------------------------------------------------------------------------------
history_db::~history_db()
{
    // Close alive handle
    CloseHandle(m_alive_file);

    // Close all but the master bank. We're going to append to the master one.
    for (int i = 1; i < sizeof_array(m_bank_handles); ++i)
        CloseHandle(m_bank_handles[i]);

    reap();

    CloseHandle(m_bank_handles[bank_master]);
}

//------------------------------------------------------------------------------
void history_db::reap()
{
    // Fold each session found that has no valid alive file.
    str<280> path;
    get_file_path(path, false);
    path << "_*";

    for (globber i(path.c_str()); i.next(path);)
    {
        path << "~";
        if (os::get_path_type(path.c_str()) == os::path_type_file)
            if (!os::unlink(path.c_str())) // abandoned alive files will unlink
                continue;

        path.truncate(path.length() - 1);

        int file_size = os::get_file_size(path.c_str());
        if (file_size > 0)
        {
            void* src_handle = open_file(path.c_str());
            {
                read_lock src(src_handle);
                write_lock dest(m_bank_handles[bank_master]);
                if (src && dest)
                    dest.append(src);
            }
            CloseHandle(src_handle);
        }

        os::unlink(path.c_str());
    }
}

//------------------------------------------------------------------------------
void history_db::initialise()
{
    if (m_bank_handles[bank_master] != nullptr)
        return;

    str<280> path;
    get_file_path(path, false);

    // Migrate existing history.
    if (os::get_path_type(path.c_str()) == os::path_type_invalid)
        migrate_history(path.c_str());

    // Open the master bank file.
    m_bank_handles[bank_master] = open_file(path.c_str());

    // Retrieve concurrency tag from start of master bank.
    m_master_ctag.clear();
    {
        read_lock lock(m_bank_handles[bank_master], false);
        extract_ctag(lock, m_master_ctag);
    }

    // No concurrency tag?  Inject one.
    if (m_master_ctag.empty())
    {
        write_lock lock(m_bank_handles[bank_master]);
        if (!extract_ctag(lock, m_master_ctag))
        {
            rewrite_master_bank(lock, max_line_length);
            extract_ctag(lock, m_master_ctag);
        }
    }
    LOG("master bank ctag: %s", m_master_ctag.get());

    if (g_shared.get())
        return;

    get_file_path(path, true);
    m_bank_handles[bank_session] = open_file(path.c_str());

    reap(); // collects orphaned history files.
}

//------------------------------------------------------------------------------
unsigned int history_db::get_active_bank() const
{
    return g_shared.get() ? bank_master : bank_session;
}

//------------------------------------------------------------------------------
void* history_db::get_bank(unsigned int index) const
{
    if (index >= sizeof_array(m_bank_handles))
        return nullptr;

    return m_bank_handles[index];
}

//------------------------------------------------------------------------------
template <typename T> void history_db::for_each_bank(T&& callback)
{
    for (int i = 0; i < sizeof_array(m_bank_handles); ++i)
    {
        write_lock lock(get_bank(i));
        if (lock && !callback(i, lock))
            break;
    }
}

//------------------------------------------------------------------------------
template <typename T> void history_db::for_each_bank(T&& callback) const
{
    for (int i = 0; i < sizeof_array(m_bank_handles); ++i)
    {
        read_lock lock(get_bank(i));
        if (lock && !callback(i, lock))
            break;
    }
}

//------------------------------------------------------------------------------
void history_db::load_internal()
{
    clear_history();
    m_index_map.clear();
    m_master_len = 0;
    m_master_deleted_count = 0;

    char buffer[max_line_length];

    const history_db& const_this = *this;
    const_this.for_each_bank([&] (unsigned int bank_index, const read_lock& lock)
    {
        if (bank_index == bank_master)
        {
            m_master_ctag.clear();
            extract_ctag(lock, m_master_ctag);
        }

        str_iter out;
        read_lock::line_iter iter(lock, buffer, sizeof_array(buffer));
        line_id_impl id;
        while (id = iter.next(out))
        {
            const char* line = out.get_pointer();
            int buffer_offset = int(line - buffer);
            buffer[buffer_offset + out.length()] = '\0';
            add_history(line);

            id.bank_index = bank_index;
            m_index_map.push_back(id.outer);
            if (bank_index == bank_master)
            {
                //LOG("load:  bank %u, offset %u, active %u:  '%s', len %u", id.bank_index, id.offset, id.active, line, out.length());
                m_master_len = m_index_map.size();
            }
        }

        if (bank_index == bank_master)
            m_master_deleted_count = iter.get_deleted_count();

        return true;
    });
}

//------------------------------------------------------------------------------
void history_db::load_rl_history(bool can_clean)
{
    load_internal();

    // The `clink history` command needs to be able to avoid cleaning the master
    // history file.
    if (can_clean)
    {
        compact();
        load_internal();
    }
}

//------------------------------------------------------------------------------
void history_db::clear()
{
    for_each_bank([&] (unsigned int bank_index, write_lock& lock)
    {
        lock.clear();
        if (bank_index == bank_master)
        {
            m_master_ctag.clear();
            m_master_ctag.generate_new_tag();
            lock.add(m_master_ctag.get());
        }
        return true;
    });

    m_index_map.clear();
    m_master_len = 0;
    m_master_deleted_count = 0;
}

//------------------------------------------------------------------------------
void history_db::compact(bool force)
{
    size_t limit = get_max_history();
    if (limit > 0)
    {
        LOG("History:  %u active, %u deleted", m_master_len, m_master_deleted_count);

        // Delete oldest history entries that exceed it.  This only marks them as
        // deleted; compacting is a separate operation.
        if (m_master_len > limit)
        {
            int removed = 0;
            while (m_master_len > limit)
            {
                line_id_impl id;
                id.outer = m_index_map[0];
                if (id.bank_index != bank_master)
                {
                    LOG("tried to trim from non-master bank");
                    break;
                }
                //LOG("remove bank %u, offset %u, active %u (master len was %u)", id.bank_index, id.offset, id.active, m_master_len);
                if (!remove(m_index_map[0]))
                {
                    LOG("failed to remove");
                    break;
                }
                removed++;
            }
            LOG("History:  removed %u", removed);
        }
    }

    // Since the ratio of deleted lines to active lines is already known here,
    // this is the most convenient/performant place to compact the master bank.
    size_t threshold = (force ? 0 :
                        limit ? max(limit, m_min_compact_threshold) :
                        2500);
    if (m_master_deleted_count > threshold)
    {
        write_lock lock(m_bank_handles[bank_master]);
        rewrite_master_bank(lock, max_line_length);
        LOG("Compacted history:  %u active, %u deleted", m_master_len, m_master_deleted_count);
    }
}

//------------------------------------------------------------------------------
bool history_db::add(const char* line)
{
    // Ignore empty and/or whitespace prefixed lines?
    if (!line[0] || (g_ignore_space.get() && (line[0] == ' ' || line[0] == '\t')))
        return false;

    // Handle duplicates.
    switch (g_dupe_mode.get())
    {
    case 1:
        // 'ignore'
        if (line_id find_result = find(line))
            return true;
        break;

    case 2:
        // 'erase_prev'
        remove(line);
        break;
    }

    // Add the line.
    void* handle = get_bank(get_active_bank());
    write_lock lock(handle);
    if (!lock)
        return false;

    lock.add(line);
    return true;
}

//------------------------------------------------------------------------------
int history_db::remove(const char* line)
{
    int count = 0;
    for_each_bank([line, &count] (unsigned int index, write_lock& lock)
    {
        lock.find(line, [&] (line_id_impl id) {
            // The line id was retrieved inside this lock scope, so it's still
            // valid; no need to guard the ctag.
            lock.remove(id);
            count++;
            return true;
        });

        return true;
    });

    return count;
}

//------------------------------------------------------------------------------
bool history_db::remove_internal(line_id id, bool guard_ctag)
{
    if (!id)
    {
        LOG("blank history id");
        return false;
    }

    line_id_impl id_impl;
    id_impl.outer = id;

    void* handle = get_bank(id_impl.bank_index);
    write_lock lock(handle);
    if (!lock)
    {
        ERR("couldn't lock");
        return false;
    }

    if (guard_ctag && id_impl.bank_index == bank_master)
    {
        concurrency_tag tag;
        if (!extract_ctag(lock, tag))
        {
            LOG("no ctag");
            return false;
        }
        if (strcmp(tag.get(), m_master_ctag.get()) != 0)
        {
            LOG("ctag '%s' doesn't match '%s'", tag.get(), m_master_ctag.get());
            return false;
        }
    }

    lock.remove(id_impl);

    if (id_impl.bank_index == bank_master)
    {
        auto last = m_index_map.begin() + m_master_len;
        auto nth = std::lower_bound(m_index_map.begin(), last, id);
        if (nth != last && id == *nth)
        {
            m_index_map.erase(nth);
            --m_master_len;
            ++m_master_deleted_count;
        }
        else
            assert(m_index_map.empty()); // Index map is empty when using `clink history delete`.
    }
    else
    {
        auto first = m_index_map.begin() + m_master_len;
        auto nth = std::lower_bound(first, m_index_map.end(), id);
        if (nth != m_index_map.end() && id == *nth)
            m_index_map.erase(nth);
        else
            assert(m_index_map.empty()); // Index map is empty when using `clink history delete`.
    }

    return true;
}

//------------------------------------------------------------------------------
bool history_db::remove(int rl_history_index, const char* line)
{
    if (rl_history_index < 0 || size_t(rl_history_index) >= m_index_map.size())
        return false;

#ifdef CLINK_DEBUG
    // Verify the file content matches the expected state.
    line_id_impl id_impl;
    id_impl.outer = m_index_map[rl_history_index];
    {
        read_lock lock(get_bank(id_impl.bank_index));
        assert(lock);

        str_iter out;
        char buffer[max_line_length];
        read_lock::line_iter iter(lock, buffer, sizeof_array(buffer));

        iter.set_file_offset(id_impl.offset);
        assert(iter.next(out));

        assert(out.length());
        assert(strlen(line) == out.length());
        if (*out.get_pointer() != '|')
            assert(memcmp(line, out.get_pointer(), out.length()) == 0);
    }
#endif

    return remove(m_index_map[rl_history_index]);
}

//------------------------------------------------------------------------------
history_db::line_id history_db::find(const char* line) const
{
    line_id_impl ret;

    for_each_bank([line, &ret] (unsigned int index, const read_lock& lock)
    {
        if (ret = lock.find(line))
            ret.bank_index = index;
        return !ret;
    });

    return ret.outer;
}

//------------------------------------------------------------------------------
history_db::expand_result history_db::expand(const char* line, str_base& out)
{
    using_history();

    char* expanded = nullptr;
    int result = history_expand((char*)line, &expanded);
    if (result >= 0 && expanded != nullptr)
        out.copy(expanded);

    free(expanded);
    return expand_result(result);
}

//------------------------------------------------------------------------------
history_db::iter history_db::read_lines(char* buffer, unsigned int size)
{
    iter ret;
    if (size > sizeof(read_line_iter))
        ret.impl = uintptr_t(new (buffer) read_line_iter(*this, size));

    return ret;
}
