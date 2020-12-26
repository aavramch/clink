// Copyright (c) 2016 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "rl_module.h"
#include "rl_commands.h"
#include "line_buffer.h"
#include "line_state.h"
#include "matches.h"
#include "match_pipeline.h"
#include "popup.h"

#include <core/base.h>
#include <core/os.h>
#include <core/path.h>
#include <core/str_hash.h>
#include <core/settings.h>
#include <core/log.h>
#include <terminal/ecma48_iter.h>
#include <terminal/printer.h>
#include <terminal/terminal_in.h>
#include <terminal/key_tester.h>
#include <terminal/screen_buffer.h>

#include <unordered_set>

extern "C" {
#include <readline/readline.h>
#include <readline/rldefs.h>
#include <readline/histlib.h>
#include <readline/keymaps.h>
#include <readline/xmalloc.h>
#include <compat/dirent.h>
#include <compat/display_matches.h>
#include <readline/posixdir.h>
#include <readline/history.h>
extern int _rl_bell_preference;
extern int _rl_match_hidden_files;
extern int _rl_history_point_at_end_of_anchored_search;
extern int rl_complete_with_tilde_expansion;
extern void _rl_reset_completion_state(void);
extern void _rl_free_match_list(char** list);
extern void rl_history_search_reinit(int flags);
extern void make_history_line_current(HIST_ENTRY *);
#define HIDDEN_FILE(fn) ((fn)[0] == '.')
#if defined (COLOR_SUPPORT)
#include <readline/parse-colors.h>
extern int _rl_colored_stats;
extern int _rl_colored_completion_prefix;
#endif
}

//------------------------------------------------------------------------------
static FILE*        null_stream = (FILE*)1;
static FILE*        in_stream = (FILE*)2;
static FILE*        out_stream = (FILE*)3;
extern "C" int      mk_wcwidth(char32_t);
extern "C" char*    tgetstr(char*, char**);
static const int    RL_MORE_INPUT_STATES = ~(
                        RL_STATE_CALLBACK|
                        RL_STATE_INITIALIZED|
                        RL_STATE_OVERWRITE|
                        RL_STATE_VICMDONCE);

extern "C" {
extern void         (*rl_fwrite_function)(FILE*, const char*, int);
extern void         (*rl_fflush_function)(FILE*);
extern char*        _rl_comment_begin;
extern int          _rl_convert_meta_chars_to_ascii;
extern int          _rl_output_meta_chars;
#if defined(PLATFORM_WINDOWS)
extern int          _rl_vis_botlin;
extern int          _rl_last_c_pos;
extern int          _rl_last_v_pos;
#endif
} // extern "C"

inline int clink_wcwidth(char32_t c)
{
    if (c >= ' ' && c <= '~')
        return 1;
    return mk_wcwidth(c);
}

extern void host_add_history(int rl_history_index, const char* line);
extern void host_remove_history(int rl_history_index, const char* line);
extern void sort_match_list(char** matches, int len);
extern matches* maybe_regenerate_matches(const char* needle, bool popup);
extern setting_color g_color_interact;

terminal_in*        s_direct_input = nullptr;       // for read_key_hook
terminal_in*        s_processed_input = nullptr;    // for read thunk
printer*            g_printer = nullptr;
line_buffer*        rl_buffer = nullptr;
pager*              g_pager = nullptr;
editor_module::result* g_result = nullptr;

static bool         s_is_popup = false;

//------------------------------------------------------------------------------
setting_color g_color_input(
    "color.input",
    "Input text color",
    "Used when Clink displays the input line text.",
    "");

setting_color g_color_modmark(
    "color.modmark",
    "Modified history line mark color",
    "Used when Clink displays the * mark on modified history lines when\n"
    "mark-modified-lines is set and color.input is set.",
    "");

setting_color g_color_message(
    "color.message",
    "Message area color",
    "The color for the Readline message area (e.g. search prompt, etc).",
    "default");

setting_color g_color_prompt(
    "color.prompt",
    "Prompt color",
    "When set, this is used as the default color for the prompt.  But it's\n"
    "overridden by any colors set by prompt filter scripts.",
    "");

setting_color g_color_hidden(
    "color.hidden",
    "Hidden file completions",
    "Used when Clink displays file completions with the hidden attribute.",
    "");

setting_color g_color_readonly(
    "color.readonly",
    "Readonly file completions",
    "Used when Clink displays file completions with the readonly attribute.",
    "");

setting_color g_color_cmd(
    "color.cmd",
    "Shell command completions",
    "Used when Clink displays shell (CMD.EXE) command completions.",
    "bold");

setting_color g_color_doskey(
    "color.doskey",
    "Doskey completions",
    "Used when Clink displays doskey macro completions.",
    "bright cyan");

setting_color g_color_filtered(
    "color.filtered",
    "Filtered completion color",
    "The default color for filtered completions.",
    "bold");

setting_bool g_match_wild(
    "match.wild",
    "menu-complete matches ? and * wildcards",
    "Matches ? and * wildcards when using any of the menu-complete commands.\n"
    "Turn this off to behave how bash does.",
    true);

setting_bool g_rl_hide_stderr(
    "readline.hide_stderr",
    "Suppress stderr from the Readline library",
    false);



//------------------------------------------------------------------------------
static void load_user_inputrc()
{
#if defined(PLATFORM_WINDOWS)
    // Remember to update clink_info() if anything changes in here.

    static const char* const env_vars[] = {
        "clink_inputrc",
        "userprofile",
        "localappdata",
        "appdata",
        "home"
    };

    static const char* const file_names[] = {
        ".inputrc",
        "_inputrc",
        "clink_inputrc",
    };

    for (const char* env_var : env_vars)
    {
        str<MAX_PATH> path;
        int path_length = GetEnvironmentVariable(env_var, path.data(), path.size());
        if (!path_length || path_length > int(path.size()))
            continue;

        path << "\\";
        int base_len = path.length();

        for (int j = 0; j < sizeof_array(file_names); ++j)
        {
            path.truncate(base_len);
            path::append(path, file_names[j]);

            if (!rl_read_init_file(path.c_str()))
            {
                LOG("Found Readline inputrc at '%s'", path.c_str());
                break;
            }
        }
    }
#endif // PLATFORM_WINDOWS
}

//------------------------------------------------------------------------------
extern "C" const char* host_get_env(const char* name)
{
    static int rotate = 0;
    static str<> rotating_tmp[10];

    str<>& s = rotating_tmp[rotate];
    rotate = (rotate + 1) % sizeof_array(rotating_tmp);
    if (!os::get_env(name, s))
        return nullptr;
    return s.c_str();
}

//------------------------------------------------------------------------------
static const char* build_color_sequence(const setting_color& setting, str_base& out, bool include_csi = false)
{
    if (include_csi)
    {
        str<> tmp;
        setting.get(tmp);
        if (tmp.empty())
            return nullptr;
        out.clear();
        out << "\x1b[" << tmp.c_str() << "m"; // Can't use format() because it DOESN'T GROW!
    }
    else
    {
        setting.get(out);
    }
    if (out.empty())
        return nullptr;
    return out.c_str();
}

//------------------------------------------------------------------------------
class rl_more_key_tester : public key_tester
{
public:
    virtual bool    is_bound(const char* seq, int len) override
                    {
                        if (len <= 1)
                            return true;
                        // Unreachable; gets handled by translate.
                        assert(strcmp(seq, bindableEsc) != 0);
                        rl_ding();
                        return false;
                    }
    virtual bool    translate(const char* seq, int len, str_base& out) override
                    {
                        if (strcmp(seq, bindableEsc) == 0)
                        {
                            out = "\x1b";
                            return true;
                        }
                        return false;
                    }
};
extern "C" int read_key_hook(void)
{
    assert(s_direct_input);
    if (!s_direct_input)
        return 0;

    rl_more_key_tester tester;
    key_tester* old = s_direct_input->set_key_tester(&tester);

    s_direct_input->select();
    int key = s_direct_input->read();

    s_direct_input->set_key_tester(old);
    return key;
}



//------------------------------------------------------------------------------
static const matches* s_matches = nullptr;

//------------------------------------------------------------------------------
static int complete_fncmp(const char *convfn, int convlen, const char *filename, int filename_len)
{
    // We let the OS handle wildcards, so not much to do here.  And we ignore
    // _rl_completion_case_fold because (1) this is Windows and (2) the
    // alternative is to write our own wildcard matching implementation.
    return 1;
}

//------------------------------------------------------------------------------
static char adjust_completion_word(char quote_char, int *found_quote, int *delimiter)
{
    if (s_matches)
    {
        // When the point immediately follows a closed quoted substring, move to
        // the beginning of the closed quoted substring.  Otherwise Readline
        // completes `"abc def"#` to `"abc def"abc def" #`.
        //
        // This goes before handling get_word_break_adjustment() because Clink
        // told the match generators the last word was this quoted substring, so
        // this brings Readline and Clink into alignment.
        if (*found_quote == RL_QF_DOUBLE_QUOTE && !*delimiter && !quote_char && rl_point > 0)
        {
            const char* pqc = strchr(rl_completer_quote_characters, rl_line_buffer[rl_point - 1]);
            if (pqc)
            {
                quote_char = *pqc;
                rl_point--;
                while (--rl_point && rl_line_buffer[rl_point] != quote_char)
                {}

                *found_quote = true;
            }
        }

        // Apply any word break adjustment from the match generators.
        int adj = min(s_matches->get_word_break_adjustment(), rl_end - rl_point);
        if (adj)
        {
            rl_point += adj;

            // TODO: Is throwing away quote state correct?
            quote_char = 0;
            *found_quote = 0;
            *delimiter = 0;
        }
    }

    return quote_char;
}

//------------------------------------------------------------------------------
static int is_exec_ext(const char* ext)
{
    return path::is_executable_extension(ext);
}

//------------------------------------------------------------------------------
static char* filename_menu_completion_function(const char *text, int state)
{
    // This function should be unreachable.
    assert(false);
    return nullptr;
}

//------------------------------------------------------------------------------
static bool ensure_matches_size(char**& matches, int count, int& reserved)
{
    count += 2;
    if (count > reserved)
    {
        int new_reserve = 64;
        while (new_reserve < count)
        {
            int prev = new_reserve;
            new_reserve <<= 1;
            if (new_reserve < prev)
                return false;
        }
        char **new_matches = (char **)realloc(matches, new_reserve * sizeof(matches[0]));
        if (!new_matches)
            return false;

        matches = new_matches;
        reserved = new_reserve;
    }
    return true;
}

//------------------------------------------------------------------------------
static char** alternative_matches(const char* text, int start, int end)
{
    rl_attempted_completion_over = 1;

    if (!s_matches)
        return nullptr;

    if (matches* regen = maybe_regenerate_matches(text, s_is_popup))
        s_matches = regen;

    str<> tmp;
    const char* pattern = nullptr;
    if (g_match_wild.get() && rl_completion_type == '%')
    {
        tmp = text;
        tmp.concat("*");
        pattern = tmp.c_str();
    }

    matches_iter iter = s_matches->get_iter(pattern);
    if (!iter.next())
        return nullptr;

    rl_completion_matches_include_type = 1;

#ifdef DEBUG
    const int debug_matches = dbg_get_env_int("DEBUG_MATCHES");
#endif

    // Identify common prefix.
    char* end_prefix = rl_last_path_separator(text);
    if (end_prefix)
        end_prefix++;
    else if (ISALPHA((unsigned char)text[0]) && text[1] == ':')
        end_prefix = (char*)text + 2;
    int len_prefix = end_prefix ? end_prefix - text : 0;

    // Deep copy of the generated matches.  Inefficient, but this is how
    // readline wants them.
    str<32> lcd;
    int past_flag = rl_completion_matches_include_type;
    int count = 0;
    int reserved = 0;
    char** matches = nullptr;
    if (!ensure_matches_size(matches, s_matches->get_match_count(), reserved))
        return nullptr;
    matches[0] = (char*)malloc(past_flag + (end - start) + 1);
    if (past_flag)
        matches[0][0] = (char)match_type::none;
    memcpy(matches[0] + past_flag, text, end - start);
    matches[0][past_flag + (end - start)] = '\0';
    do
    {
        match_type type = past_flag ? iter.get_match_type() : match_type::none;
        match_type masked_type = type & match_type::mask;

        ++count;
        if (!ensure_matches_size(matches, count, reserved))
        {
            --count;
            break;
        }

        const char* match = iter.get_match();
        int match_len = strlen(match);
        int match_size = past_flag + match_len + 1;
        matches[count] = (char*)malloc(match_size);

        if (past_flag)
            matches[count][0] = (char)type;

        str_base str(matches[count] + past_flag, match_size - past_flag);
        str.clear();
        str.concat(match, match_len);

#ifdef DEBUG
        if (debug_matches > 0 || (debug_matches < 0 && count - 1 < 0 - debug_matches))
            printf("%u: %s, %02.2x => %s\n", count - 1, match, type, matches[count] + past_flag);
#endif
    }
    while (iter.next());
    matches[count + 1] = nullptr;

    switch (s_matches->get_suppress_quoting())
    {
    case 1: rl_filename_quoting_desired = 0; break;
    case 2: rl_completion_suppress_quote = 1; break;
    }

    rl_completion_suppress_append = s_matches->is_suppress_append();
    if (s_matches->get_append_character())
        rl_completion_append_character = s_matches->get_append_character();

    rl_filename_completion_desired = iter.is_filename_completion_desired();
    rl_filename_display_desired = iter.is_filename_display_desired();

#ifdef DEBUG
    if (debug_matches)
    {
        printf("count = %d\n", count);
        printf("filename completion desired = %d (%s)\n", rl_filename_completion_desired, iter.is_filename_completion_desired().is_explicit() ? "explicit" : "implicit");
        printf("filename display desired = %d (%s)\n", rl_filename_display_desired, iter.is_filename_display_desired().is_explicit() ? "explicit" : "implicit");
        printf("is suppress append = %d\n", s_matches->is_suppress_append());
        printf("get append character = %u\n", (unsigned char)s_matches->get_append_character());
        printf("get suppress quoting = %d\n", s_matches->get_suppress_quoting());
    }
#endif

    return matches;
}

//------------------------------------------------------------------------------
struct match_hasher
{
    size_t operator()(const char* match) const
    {
        return str_hash(match);
    }
};

//------------------------------------------------------------------------------
struct match_comparator
{
    bool operator()(const char* m1, const char* m2) const
    {
        return strcmp(m1, m2) == 0;
    }
};

//------------------------------------------------------------------------------
static match_display_filter_entry** match_display_filter(char** matches, bool popup)
{
    if (!s_matches)
        return nullptr;

    match_display_filter_entry** filtered_matches = nullptr;
    if (!s_matches->match_display_filter(matches, &filtered_matches, popup))
        return nullptr;

    // Remove duplicates.
    if (filtered_matches[0] && filtered_matches[1])
    {
#ifdef DEBUG
        const int debug_filter = dbg_get_env_int("DEBUG_FILTER");
#endif

        std::unordered_set<const char*, match_hasher, match_comparator> seen;
        unsigned int tortoise = 1;
        unsigned int hare = 1;
        while (filtered_matches[hare])
        {
            const char* display = filtered_matches[hare]->display;
            if (!display || !*display)
                display = filtered_matches[hare]->match;
            if (seen.find(display) != seen.end())
            {
#ifdef DEBUG
                if (debug_filter)
                    printf("%u dupe: %s\n", hare, display);
#endif
                free(filtered_matches[hare]);
            }
            else
            {
#ifdef DEBUG
                if (debug_filter)
                    printf("%u->%u: %s\n", hare, tortoise, display);
#endif
                seen.insert(display);
                if (hare > tortoise)
                    filtered_matches[tortoise] = filtered_matches[hare];
                tortoise++;
            }
            hare++;
        }
        filtered_matches[tortoise] = nullptr;
    }

    return filtered_matches;
}

//------------------------------------------------------------------------------
static match_display_filter_entry** match_display_filter_callback(char** matches)
{
    return match_display_filter(matches, false/*popup*/);
}

//------------------------------------------------------------------------------
// If the input text starts with a slash and doesn't have any other slashes or
// path separators, then preserve the original slash in the lcd.  Otherwise it
// converts "somecommand /" to "somecommand \" and we lose the ability to try
// completing to test if an argmatcher has defined flags for "somecommand".
static void postprocess_lcd(char* lcd, const char* text)
{
    if (*text != '/')
        return;

    while (*(++text))
        if (*text == '/' || rl_is_path_separator(*text))
            return;

    lcd[!!rl_completion_matches_include_type] = '/';
}

//------------------------------------------------------------------------------
int clink_popup_complete(int count, int invoking_key)
{
    if (!s_matches)
    {
        rl_ding();
        return 0;
    }

    rl_completion_invoking_key = invoking_key;

    // Collect completions.
    int match_count;
    char* orig_text;
    int orig_start;
    int orig_end;
    int delimiter;
    char quote_char;
    bool completing = true;
    bool free_match_strings = true;
    rollback<bool> popup_scope(s_is_popup, true);
    char** matches = rl_get_completions(&match_count, &orig_text, &orig_start, &orig_end, &delimiter, &quote_char);
    if (!matches)
        return 0;
    int past_flag = rl_completion_matches_include_type ? 1 : 0;

    // Identify common prefix.
    char* end_prefix = rl_last_path_separator(orig_text);
    if (end_prefix)
        end_prefix++;
    else if (ISALPHA((unsigned char)orig_text[0]) && orig_text[1] == ':')
        end_prefix = (char*)orig_text + 2;
    int len_prefix = end_prefix ? end_prefix - orig_text : 0;

    // Match display filter.
    bool any_descriptions = false;
    match_display_filter_entry** filtered_matches = match_display_filter(matches, true/*popup*/);
    if (filtered_matches && filtered_matches[0] && filtered_matches[1])
    {
        _rl_free_match_list(matches);
        free_match_strings = false;
        matches = nullptr;

        completing = false;
        past_flag = 0;

        match_count = 0;
        for (int i = 1; filtered_matches[i]; i++)
        {
            any_descriptions = any_descriptions || filtered_matches[i]->description;
            match_count++;
        }

        if (match_count)
        {
            matches = (char**)calloc(match_count + 1, sizeof(*matches));
            if (matches)
            {
                for (int i = 1; filtered_matches[i]; i++)
                    matches[i - 1] = const_cast<char*>(filtered_matches[i]->buffer);
                matches[match_count] = nullptr;
            }
        }
    }

    // Popup list.
    int current = 0;
    str<32> choice;
    switch (do_popup_list("Completions", (const char **)matches, match_count,
                          len_prefix, past_flag, completing,
                          true/*auto_complete*/, current, choice, any_descriptions))
    {
    case popup_list_result::cancel:
        break;
    case popup_list_result::error:
        rl_ding();
        break;
    case popup_list_result::select:
    case popup_list_result::use:
        {
            rollback<int> rb(rl_completion_matches_include_type, past_flag);
            rl_insert_match(choice.data(), orig_text, orig_start, delimiter, quote_char);
        }
        break;
    }

    _rl_reset_completion_state();

    free(orig_text);
    if (free_match_strings)
        _rl_free_match_list(matches);
    else
        free(matches);
    free_filtered_matches(filtered_matches);

    return 0;
}

//------------------------------------------------------------------------------
int clink_popup_history(int count, int invoking_key)
{
    HIST_ENTRY** list = history_list();
    if (!list || !history_length)
    {
        rl_ding();
        return 0;
    }

    rl_completion_invoking_key = invoking_key;
    rl_completion_matches_include_type = 0;

    int current = -1;
    int orig_pos = where_history();
    int search_len = rl_point;

    // Copy the history list (just a shallow copy of the line pointers).
    char** history = (char**)malloc(sizeof(*history) * history_length);
    int* indices = (int*)malloc(sizeof(*indices) * history_length);
    int total = 0;
    for (int i = 0; i < history_length; i++)
    {
        if (!STREQN(rl_buffer->get_buffer(), list[i]->line, search_len))
            continue;
        history[total] = list[i]->line;
        indices[total] = i;
        if (i == orig_pos)
            current = total;
        total++;
    }
    if (!total)
    {
        rl_ding();
        free(history);
        free(indices);
        return 0;
    }
    if (current < 0)
        current = total - 1;

    // Popup list.
    str<> choice;
    popup_list_result result = do_popup_list("History",
        (const char **)history, total, 0, 0,
        false/*completing*/, false/*auto_complete*/, current, choice);
    switch (result)
    {
    case popup_list_result::cancel:
        break;
    case popup_list_result::error:
        rl_ding();
        break;
    case popup_list_result::select:
    case popup_list_result::use:
        {
            HIST_ENTRY *temp = nullptr;
            int oldpos;

            current = indices[current];

            oldpos = where_history();
            history_set_pos(current);
            rl_history_search_reinit(ANCHORED_SEARCH);
            temp = current_history();
            history_set_pos(oldpos);

            rl_maybe_save_line();

            make_history_line_current(temp);

            bool point_at_end = (!search_len || _rl_history_point_at_end_of_anchored_search);
            rl_point = point_at_end ? rl_end : search_len;
            rl_mark = point_at_end ? search_len : rl_end;

            if (result == popup_list_result::use)
            {
                rl_redisplay();
                rl_newline(1, invoking_key);
            }
        }
        break;
    }

    free(history);
    free(indices);

    return 0;
}



//------------------------------------------------------------------------------
enum {
    bind_id_input,
    bind_id_more_input,
};



//------------------------------------------------------------------------------
static int terminal_read_thunk(FILE* stream)
{
    if (stream == in_stream)
    {
        assert(s_processed_input);
        return s_processed_input->read();
    }

    if (stream == null_stream)
        return 0;

    assert(false);
    return fgetc(stream);
}

//------------------------------------------------------------------------------
static void terminal_write_thunk(FILE* stream, const char* chars, int char_count)
{
    if (stream == out_stream)
    {
        assert(g_printer);
        g_printer->print(chars, char_count);
        return;
    }

    if (stream == null_stream)
        return;

    if (stream == stderr || stream == stdout)
    {
        if (stream == stderr && g_rl_hide_stderr.get())
            return;

        DWORD dw;
        HANDLE h = GetStdHandle(stream == stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
        if (GetConsoleMode(h, &dw))
        {
            wstr<32> s;
            to_utf16(s, str_iter(chars, char_count));
            WriteConsoleW(h, s.c_str(), s.length(), &dw, nullptr);
        }
        else
        {
            WriteFile(h, chars, char_count, &dw, nullptr);
        }
        return;
    }

    assert(false);
    fwrite(chars, char_count, 1, stream);
}

//------------------------------------------------------------------------------
static void terminal_fflush_thunk(FILE* stream)
{
    if (stream != out_stream && stream != null_stream)
        fflush(stream);
}

//------------------------------------------------------------------------------
typedef const char* two_strings[2];
static void bind_keyseq_list(const two_strings* list, Keymap map)
{
    for (int i = 0; list[i][0]; ++i)
        rl_bind_keyseq_in_map(list[i][0], rl_named_function(list[i][1]), map);
}



//------------------------------------------------------------------------------
rl_module::rl_module(const char* shell_name, terminal_in* input)
: m_rl_buffer(nullptr)
, m_prev_group(-1)
{
    assert(!s_direct_input);
    s_direct_input = input;

    rl_getc_function = terminal_read_thunk;
    rl_fwrite_function = terminal_write_thunk;
    rl_fflush_function = terminal_fflush_thunk;
    rl_instream = in_stream;
    rl_outstream = out_stream;
    _rl_visual_bell_func = visible_bell;

    rl_readline_name = shell_name;
    rl_catch_signals = 0;

    // Readline needs a tweak of it's handling of 'meta' (i.e. IO bytes >=0x80)
    // so that it handles UTF-8 correctly (convert=input, output=output)
    _rl_convert_meta_chars_to_ascii = 0;
    _rl_output_meta_chars = 1;

    // Recognize both / and \\ as path separators, and normalize to \\.
    rl_backslash_path_sep = 1;
    rl_preferred_path_separator = PATH_SEP[0];

    // Quote spaces in completed filenames.
    rl_completer_quote_characters = "\"";
    rl_basic_quote_characters = "\"";

    // Same list CMD uses for quoting filenames.
    rl_filename_quote_characters = " &()[]{}^=;!%'+,`~";

    // Word break characters -- equal to rl_basic_word_break_characters, with
    // backslash removed (because rl_backslash_path_sep) and without '$' or '%'
    // so we can let the match generators decide when '%' should start a word or
    // end a word (see :getwordbreakinfo()).
    rl_completer_word_break_characters = " \t\n\"'`@><=;|&{("; /* }) */

    // Completion and match display.
    // TODO: postprocess_matches is for better quote handling.
    //rl_ignore_some_completions_function = postprocess_matches;
    rl_attempted_completion_function = alternative_matches;
    rl_menu_completion_entry_function = filename_menu_completion_function;
    rl_adjust_completion_word = adjust_completion_word;
    rl_completion_display_matches_func = display_matches;
    rl_qsort_match_list_func = sort_match_list;
    rl_match_display_filter_func = match_display_filter_callback;
    rl_is_exec_func = is_exec_ext;
    rl_postprocess_lcd_func = postprocess_lcd;
    rl_read_key_hook = read_key_hook;

    // Add commands.
    static bool s_rl_initialized = false;
    if (!s_rl_initialized)
    {
        s_rl_initialized = true;

        rl_add_history_hook = host_add_history;
        rl_remove_history_hook = host_remove_history;
        rl_add_funmap_entry("clink-reload", clink_reload);
        rl_add_funmap_entry("clink-reset-line", clink_reset_line);
        rl_add_funmap_entry("clink-show-help", show_rl_help);
        rl_add_funmap_entry("clink-show-help-raw", show_rl_help_raw);
        rl_add_funmap_entry("clink-exit", clink_exit);
        rl_add_funmap_entry("clink-ctrl-c", clink_ctrl_c);
        rl_add_funmap_entry("clink-paste", clink_paste);
        rl_add_funmap_entry("clink-copy-line", clink_copy_line);
        rl_add_funmap_entry("clink-copy-word", clink_copy_word);
        rl_add_funmap_entry("clink-copy-cwd", clink_copy_cwd);
        rl_add_funmap_entry("clink-up-directory", clink_up_directory);
        rl_add_funmap_entry("clink-insert-dot-dot", clink_insert_dot_dot);
        rl_add_funmap_entry("clink-scroll-line-up", clink_scroll_line_up);
        rl_add_funmap_entry("clink-scroll-line-down", clink_scroll_line_down);
        rl_add_funmap_entry("clink-scroll-page-up", clink_scroll_page_up);
        rl_add_funmap_entry("clink-scroll-page-down", clink_scroll_page_down);
        rl_add_funmap_entry("clink-scroll-top", clink_scroll_top);
        rl_add_funmap_entry("clink-scroll-bottom", clink_scroll_bottom);
        rl_add_funmap_entry("clink-popup-complete", clink_popup_complete);
        rl_add_funmap_entry("clink-popup-history", clink_popup_history);
        rl_add_funmap_entry("clink-popup-directories", clink_popup_directories);

        // Override some defaults.
        _rl_bell_preference = VISIBLE_BELL;     // Because audible is annoying.
        _rl_comment_begin = savestring("::");   // this will do...
        rl_complete_with_tilde_expansion = 1;   // Since CMD doesn't understand tilde.
    }

    // Bind extended keys so editing follows Windows' conventions.
    static const char* emacs_key_binds[][2] = {
        { "\\e[1;5D",       "backward-word" },           // ctrl-left
        { "\\e[1;5C",       "forward-word" },            // ctrl-right
        { "\\e[F",          "end-of-line" },             // end
        { "\\e[H",          "beginning-of-line" },       // home
        { "\\e[3~",         "delete-char" },             // del
        { "\\e[1;5F",       "kill-line" },               // ctrl-end
        { "\\e[1;5H",       "backward-kill-line" },      // ctrl-home
        { "\\e[5~",         "history-search-backward" }, // pgup
        { "\\e[6~",         "history-search-forward" },  // pgdn
        { "\\e[3;5~",       "kill-word" },               // ctrl-del
        { "\\d",            "backward-kill-word" },      // ctrl-backspace
        { "\\e[2~",         "overwrite-mode" },          // ins
        { bindableEsc,      "clink-reset-line" },        // esc
        { "\\C-c",          "clink-ctrl-c" },            // ctrl-c
        { "\\C-v",          "clink-paste" },             // ctrl-v
        { "\\C-z",          "undo" },                    // ctrl-z
        { "\\C-x\\C-r",     "clink-reload" },            // ctrl-x,ctrl-r
        {}
    };

    static const char* general_key_binds[][2] = {
        { "\\M-a",          "clink-insert-dot-dot" },    // alt-a
        { "\\M-d",          "remove-history" },          // alt-d
        { "\\M-c",          "clink-copy-cwd" },          // alt-c
        { "\\M-h",          "clink-show-help" },         // alt-h
        { "\\M-H",          "clink-show-help-raw" },     // alt-H
        { "\\M-k",          "add-history" },             // alt-k
        { "\\M-\\C-c",      "clink-copy-line" },         // alt-ctrl-c
        { "\\M-\\C-e",      "clink-expand-env-var" },    // alt-ctrl-e
        { "\\M-\\C-f",      "clink-expand-doskey-alias" }, // alt-ctrl-f
        { "\\M-\\C-w",      "clink-copy-word" },         // alt-ctrl-w
        { "\\e[5;5~",       "clink-up-directory" },      // ctrl-pgup
        { "\\e\\eOS",       "clink-exit" },              // alt-f4
        { "\\e[1;3H",       "clink-scroll-top" },        // alt-home
        { "\\e[1;3F",       "clink-scroll-bottom" },     // alt-end
        { "\\e[5;3~",       "clink-scroll-page-up" },    // alt-pgup
        { "\\e[6;3~",       "clink-scroll-page-down" },  // alt-pgdn
        { "\\e[1;3A",       "clink-scroll-line-up" },    // alt-up
        { "\\e[1;3B",       "clink-scroll-line-down" },  // alt-down
        {}
    };

    static const char* vi_insertion_key_binds[][2] = {
        { "\\M-\\C-i",      "tab-insert" },              // alt-ctrl-i
        { "\\M-\\C-j",      "emacs-editing-mode" },      // alt-ctrl-j
        { "\\M-\\C-k",      "kill-line" },               // alt-ctrl-k
        { "\\M-\\C-m",      "emacs-editing-mode" },      // alt-ctrl-m
        { bindableEsc,      "vi-movement-mode" },        // esc
        { "\\C-_",          "vi-undo-mode" },            // ctrl--
        { "\\M-0",          "vi-arg-digit" },            // alt-0
        { "\\M-1",          "vi-arg-digit" },            // alt-1
        { "\\M-2",          "vi-arg-digit" },            // alt-2
        { "\\M-3",          "vi-arg-digit" },            // alt-3
        { "\\M-4",          "vi-arg-digit" },            // alt-4
        { "\\M-5",          "vi-arg-digit" },            // alt-5
        { "\\M-6",          "vi-arg-digit" },            // alt-6
        { "\\M-7",          "vi-arg-digit" },            // alt-7
        { "\\M-8",          "vi-arg-digit" },            // alt-8
        { "\\M-9",          "vi-arg-digit" },            // alt-9
        { "\\M-[",          "arrow-key-prefix" },        // arrow key prefix
        { "\\d",            "backward-kill-word" },      // ctrl-backspace
        {}
    };

    static const char* vi_movement_key_binds[][2] = {
        { "\\M-\\C-j",      "emacs-editing-mode" },      // alt-ctrl-j
        { "\\M-\\C-m",      "emacs-editing-mode" },      // alt-ctrl-m
        {}
    };

    int restore_convert = _rl_convert_meta_chars_to_ascii;
    _rl_convert_meta_chars_to_ascii = 1;

    rl_unbind_key_in_map(' ', emacs_meta_keymap);
    bind_keyseq_list(general_key_binds, emacs_standard_keymap);
    bind_keyseq_list(emacs_key_binds, emacs_standard_keymap);

    rl_unbind_key_in_map(27, vi_insertion_keymap);
    bind_keyseq_list(general_key_binds, vi_insertion_keymap);
    bind_keyseq_list(general_key_binds, vi_movement_keymap);
    bind_keyseq_list(vi_insertion_key_binds, vi_insertion_keymap);
    bind_keyseq_list(vi_movement_key_binds, vi_movement_keymap);

    load_user_inputrc();

    _rl_convert_meta_chars_to_ascii = restore_convert;
}

//------------------------------------------------------------------------------
rl_module::~rl_module()
{
    free(_rl_comment_begin);
    _rl_comment_begin = nullptr;

    s_direct_input = nullptr;
}

//------------------------------------------------------------------------------
void rl_module::set_keyseq_len(int len)
{
    assert(m_insert_next_len == 0);
    if (rl_is_insert_next_callback_pending())
        m_insert_next_len = len;
}

//------------------------------------------------------------------------------
void rl_module::bind_input(binder& binder)
{
    int default_group = binder.get_group();
    binder.bind(default_group, "", bind_id_input);

    m_catch_group = binder.create_group("readline");
    binder.bind(m_catch_group, "", bind_id_more_input);
}

//------------------------------------------------------------------------------
void rl_module::on_begin_line(const context& context)
{
    g_printer = &context.printer;
    g_pager = &context.pager;
    rl_buffer = &context.buffer;

    // Readline needs to be told about parts of the prompt that aren't visible
    // by enclosing them in a pair of 0x01/0x02 chars.
    str<128> rl_prompt;

    bool force_prompt_color = false;
    {
        str<16> tmp;
        const char* prompt_color = build_color_sequence(g_color_prompt, tmp, true);
        if (prompt_color)
        {
            force_prompt_color = true;
            rl_prompt.format("\x01%s\x02", prompt_color);
        }
    }

    ecma48_state state;
    ecma48_iter iter(context.prompt, state);
    while (const ecma48_code& code = iter.next())
    {
        bool c1 = (code.get_type() == ecma48_code::type_c1);
        if (c1) rl_prompt.concat("\x01", 1);
                rl_prompt.concat(code.get_pointer(), code.get_length());
        if (c1) rl_prompt.concat("\x02", 1);
    }

    if (force_prompt_color)
        rl_prompt.concat("\x01\x1b[m\x02");

    _rl_display_input_color = build_color_sequence(g_color_input, m_input_color, true);
    _rl_display_modmark_color = _rl_display_input_color ? build_color_sequence(g_color_modmark, m_modmark_color, true) : nullptr;
    _rl_display_message_color = build_color_sequence(g_color_message, m_message_color, true);
    _rl_pager_color = build_color_sequence(g_color_interact, m_pager_color);
    _rl_hidden_color = build_color_sequence(g_color_hidden, m_hidden_color);
    _rl_readonly_color = build_color_sequence(g_color_readonly, m_readonly_color);
    _rl_command_color = build_color_sequence(g_color_cmd, m_command_color);
    _rl_alias_color = build_color_sequence(g_color_doskey, m_alias_color);
    _rl_filtered_color = build_color_sequence(g_color_filtered, m_filtered_color, true);

    if (!_rl_display_message_color)
        _rl_display_message_color = "\x1b[m";

    auto handler = [] (char* line) { rl_module::get()->done(line); };
    rl_callback_handler_install(rl_prompt.c_str(), handler);

    if (_rl_colored_stats || _rl_colored_completion_prefix)
        _rl_parse_colors();

    m_done = false;
    m_eof = false;
    m_prev_group = -1;
}

//------------------------------------------------------------------------------
void rl_module::on_end_line()
{
    if (m_rl_buffer != nullptr)
    {
        rl_line_buffer = m_rl_buffer;
        m_rl_buffer = nullptr;
    }

    _rl_display_input_color = nullptr;
    _rl_display_modmark_color = nullptr;
    _rl_display_message_color = nullptr;
    _rl_pager_color = nullptr;
    _rl_hidden_color = nullptr;
    _rl_readonly_color = nullptr;
    _rl_command_color = nullptr;
    _rl_alias_color = nullptr;
    _rl_filtered_color = nullptr;

    // This prevents any partial Readline state leaking from one line to the next
    rl_readline_state &= ~RL_MORE_INPUT_STATES;

    rl_buffer = nullptr;
    g_pager = nullptr;
    g_printer = nullptr;
}

//------------------------------------------------------------------------------
void rl_module::on_matches_changed(const context& context)
{
}

//------------------------------------------------------------------------------
void rl_module::on_classifications_changed(const context& context)
{
}

//------------------------------------------------------------------------------
void rl_module::on_input(const input& input, result& result, const context& context)
{
    assert(!g_result);
    g_result = &result;

    // Setup the terminal.
    struct : public terminal_in
    {
        virtual void begin() override   {}
        virtual void end() override     {}
        virtual void select() override  {}
        virtual int  read() override    { return *(unsigned char*)(data++); }
        virtual key_tester* set_key_tester(key_tester* keys) override { return nullptr; }
        const char*  data;
    } term_in;

    term_in.data = input.keys;

    terminal_in* old_input = s_processed_input;
    s_processed_input = &term_in;
    s_matches = &context.matches;

    // Call Readline's until there's no characters left.
    int is_inc_searching = rl_readline_state & RL_STATE_ISEARCH;
    unsigned int len = input.len;
    while (len && !m_done)
    {
        bool is_quoted_insert = rl_is_insert_next_callback_pending();

        --len;
        rl_callback_read_char();

        // Internally Readline tries to resend escape characters but it doesn't
        // work with how Clink uses Readline. So we do it here instead.
        if (term_in.data[-1] == 0x1b && is_inc_searching)
        {
            assert(!is_quoted_insert);
            --term_in.data;
            ++len;
            is_inc_searching = 0;
        }

        if (m_insert_next_len > 0)
        {
            if (is_quoted_insert && --m_insert_next_len)
                rl_quoted_insert(1, 0);
            else
                m_insert_next_len = 0;
        }
    }

    g_result = nullptr;
    s_matches = nullptr;
    s_processed_input = old_input;

    if (m_done)
    {
        result.done(m_eof);
        return;
    }

    // Check if Readline wants more input or if we're done.
    if (rl_readline_state & RL_MORE_INPUT_STATES)
    {
        if (m_prev_group < 0)
            m_prev_group = result.set_bind_group(m_catch_group);
    }
    else if (m_prev_group >= 0)
    {
        result.set_bind_group(m_prev_group);
        m_prev_group = -1;
    }
}

//------------------------------------------------------------------------------
void rl_module::done(const char* line)
{
    m_done = true;
    m_eof = (line == nullptr);

    // Readline will reset the line state on returning from this call. Here we
    // trick it into reseting something else so we can use rl_line_buffer later.
    static char dummy_buffer = 0;
    m_rl_buffer = rl_line_buffer;
    rl_line_buffer = &dummy_buffer;

    rl_callback_handler_remove();
}

//------------------------------------------------------------------------------
void rl_module::on_terminal_resize(int columns, int rows, const context& context)
{
#if 1
    rl_reset_screen_size();
    rl_redisplay();
#else
    static int prev_columns = columns;

    int remaining = prev_columns;
    int line_count = 1;

    auto measure = [&] (const char* input, int length) {
        ecma48_state state;
        ecma48_iter iter(input, state, length);
        while (const ecma48_code& code = iter.next())
        {
            switch (code.get_type())
            {
            case ecma48_code::type_chars:
                for (str_iter i(code.get_pointer(), code.get_length()); i.more(); )
                {
                    int n = clink_wcwidth(i.next());
                    remaining -= n;
                    if (remaining > 0)
                        continue;

                    ++line_count;

                    remaining = prev_columns - ((remaining < 0) << 1);
                }
                break;

            case ecma48_code::type_c0:
                switch (code.get_code())
                {
                case ecma48_code::c0_lf:
                    ++line_count;
                    /* fallthrough */

                case ecma48_code::c0_cr:
                    remaining = prev_columns;
                    break;

                case ecma48_code::c0_ht:
                    if (int n = 8 - ((prev_columns - remaining) & 7))
                        remaining = max(remaining - n, 0);
                    break;

                case ecma48_code::c0_bs:
                    remaining = min(remaining + 1, prev_columns); // doesn't consider full-width
                    break;
                }
                break;
            }
        }
    };

    measure(context.prompt, -1);

    const line_buffer& buffer = context.buffer;
    const char* buffer_ptr = buffer.get_buffer();
    measure(buffer_ptr, buffer.get_cursor());
    int cursor_line = line_count - 1;

    buffer_ptr += buffer.get_cursor();
    measure(buffer_ptr, -1);

    static const char* const termcap_up    = tgetstr("ku", nullptr);
    static const char* const termcap_down  = tgetstr("kd", nullptr);
    static const char* const termcap_cr    = tgetstr("cr", nullptr);
    static const char* const termcap_clear = tgetstr("ce", nullptr);

    auto& printer = context.printer;

    // Move cursor to bottom line.
    for (int i = line_count - cursor_line; --i;)
        printer.print(termcap_down, 64);

    printer.print(termcap_cr, 64);
    do
    {
        printer.print(termcap_clear, 64);

        if (--line_count)
            printer.print(termcap_up, 64);
    }
    while (line_count);

    printer.print(context.prompt, 1024);
    printer.print(buffer.get_buffer(), 1024);

    prev_columns = columns;
#endif
}
