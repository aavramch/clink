// Copyright (c) 2018 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "win_screen_buffer.h"
#include "cielab.h"

#include <core/base.h>
#include <core/log.h>
#include <core/os.h>
#include <core/settings.h>
#include <core/str_iter.h>

#include <Windows.h>
#include <assert.h>

//------------------------------------------------------------------------------
static setting_enum g_terminal_emulation(
    "terminal.emulation",
    "Controls VT emulation",
    "Clink can emulate Virtual Terminal processing if the console doesn't\n"
    "natively. When set to 'emulate' then Clink performs VT emulation and handles\n"
    "ANSI escape codes. When 'native' then Clink passes all output directly to the\n"
    "console. Or when 'auto' then Clink performs VT emulation unless native\n"
    "terminal support is detected (such as when hosted inside ConEmu or Windows\n"
    "Terminal).",
    "native,emulate,auto",
    2);

//------------------------------------------------------------------------------
win_screen_buffer::~win_screen_buffer()
{
    close();
}

//------------------------------------------------------------------------------
void win_screen_buffer::open()
{
    assert(!m_handle);
    m_handle = GetStdHandle(STD_OUTPUT_HANDLE);
}

//------------------------------------------------------------------------------
void win_screen_buffer::begin()
{
    if (!m_handle)
        open();

    GetConsoleMode(m_handle, &m_prev_mode);

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(m_handle, &csbi);
    m_default_attr = csbi.wAttributes & attr_mask_all;
    m_bold = !!(m_default_attr & attr_mask_bold);

    bool native_vt = m_native_vt;
    const char* found_what = nullptr;
    switch (g_terminal_emulation.get())
    {
    case 0:
        native_vt = true;
        break;
    case 1:
        native_vt = false;
        break;
    case 2: {
        str<16> wt_session;
        if (os::get_env("WT_SESSION", wt_session))
        {
            native_vt = true;
            found_what = "WT_SESSION";
            break;
        }

        static const char* const dll_names[] =
        {
            "conemuhk.dll",
            "conemuhk32.dll",
            "conemuhk64.dll",
            "ansi.dll",
            "ansi32.dll",
            "ansi64.dll",
        };

        native_vt = false;
        for (auto dll_name : dll_names)
        {
            if (GetModuleHandle(dll_name) != NULL)
            {
                native_vt = true;
                found_what = dll_name;
                break;
            }
        }
        break; }
    }

    if (m_native_vt != native_vt)
    {
        if (!native_vt)
            LOG("Using emulated terminal support.");
        else if (found_what)
            LOG("Using native terminal support; found '%s'.", found_what);
        else
            LOG("Using native terminal support.");
    }

    m_native_vt = native_vt;

    if (m_native_vt)
        SetConsoleMode(m_handle, m_prev_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    m_ready = true;
}

//------------------------------------------------------------------------------
void win_screen_buffer::end()
{
    if (m_ready)
    {
        SetConsoleTextAttribute(m_handle, m_default_attr);
        SetConsoleMode(m_handle, m_prev_mode);
        m_ready = false;
    }
}

//------------------------------------------------------------------------------
void win_screen_buffer::close()
{
    m_handle = nullptr;
}

//------------------------------------------------------------------------------
void win_screen_buffer::write(const char* data, int length)
{
    assert(m_ready);

    str_iter iter(data, length);
    while (length > 0)
    {
        wchar_t wbuf[384];
        int n = min<int>(sizeof_array(wbuf), length + 1);
        n = to_utf16(wbuf, n, iter);
        if (!n && !*iter.get_pointer())
        {
            assert(false); // Very inefficient, and shouldn't be possible.
            wbuf[0] = '\0';
            n = 1;
        }

#ifdef CLINK_DEBUG
        // Why was this forcing a break/crash on '\r'??
        // for (int i = 0; i < n; ++i)
        //     if (wbuf[i] == '\r')
        //         __debugbreak();
#endif

        DWORD written;
        WriteConsoleW(m_handle, wbuf, n, &written, nullptr);

        n = int(iter.get_pointer() - data);
        length -= n;
        data += n;
    }
}

//------------------------------------------------------------------------------
void win_screen_buffer::flush()
{
    // When writing to the console conhost.exe will restart the cursor blink
    // timer and hide it which can be disorientating, especially when moving
    // around a line. The below will make sure it stays visible.
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(m_handle, &csbi);
    SetConsoleCursorPosition(m_handle, csbi.dwCursorPosition);
}

//------------------------------------------------------------------------------
int win_screen_buffer::get_columns() const
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(m_handle, &csbi);
    return csbi.dwSize.X;
}

//------------------------------------------------------------------------------
int win_screen_buffer::get_rows() const
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(m_handle, &csbi);
    return (csbi.srWindow.Bottom - csbi.srWindow.Top) + 1;
}

//------------------------------------------------------------------------------
bool win_screen_buffer::has_native_vt_processing() const
{
    return m_native_vt;
}

//------------------------------------------------------------------------------
void win_screen_buffer::clear(clear_type type)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(m_handle, &csbi);

    int width, height, count = 0;
    COORD xy;

    switch (type)
    {
    case clear_type_all:
        width = csbi.dwSize.X;
        height = (csbi.srWindow.Bottom - csbi.srWindow.Top) + 1;
        xy = { 0, csbi.srWindow.Top };
        break;

    case clear_type_before:
        width = csbi.dwSize.X;
        height = csbi.dwCursorPosition.Y - csbi.srWindow.Top;
        xy = { 0, csbi.srWindow.Top };
        count = csbi.dwCursorPosition.X + 1;
        break;

    case clear_type_after:
        width = csbi.dwSize.X;
        height = csbi.srWindow.Bottom - csbi.dwCursorPosition.Y;
        xy = { csbi.dwCursorPosition.X, csbi.dwCursorPosition.Y };
        count = width - csbi.dwCursorPosition.X;
        break;
    }

    count += width * height;

    DWORD written;
    FillConsoleOutputCharacterW(m_handle, ' ', count, xy, &written);
    FillConsoleOutputAttribute(m_handle, csbi.wAttributes, count, xy, &written);
}

//------------------------------------------------------------------------------
void win_screen_buffer::clear_line(clear_type type)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(m_handle, &csbi);

    int width;
    COORD xy;
    switch (type)
    {
    case clear_type_all:
        width = csbi.dwSize.X;
        xy = { 0, csbi.dwCursorPosition.Y };
        break;

    case clear_type_before:
        width = csbi.dwCursorPosition.X + 1;
        xy = { 0, csbi.dwCursorPosition.Y };
        break;

    case clear_type_after:
        width = csbi.dwSize.X - csbi.dwCursorPosition.X;
        xy = { csbi.dwCursorPosition.X, csbi.dwCursorPosition.Y };
        break;
    }

    DWORD written;
    FillConsoleOutputCharacterW(m_handle, ' ', width, xy, &written);
    FillConsoleOutputAttribute(m_handle, csbi.wAttributes, width, xy, &written);
}

//------------------------------------------------------------------------------
void win_screen_buffer::set_cursor(int column, int row)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(m_handle, &csbi);

    const SMALL_RECT& window = csbi.srWindow;
    int width = (window.Right - window.Left) + 1;
    int height = (window.Bottom - window.Top) + 1;

    column = clamp(column, 0, width);
    row = clamp(row, 0, height);

    COORD xy = { window.Left + SHORT(column), window.Top + SHORT(row) };
    SetConsoleCursorPosition(m_handle, xy);
}

//------------------------------------------------------------------------------
void win_screen_buffer::move_cursor(int dx, int dy)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(m_handle, &csbi);

    COORD xy = {
        SHORT(clamp(csbi.dwCursorPosition.X + dx, 0, csbi.dwSize.X - 1)),
        SHORT(clamp(csbi.dwCursorPosition.Y + dy, 0, csbi.dwSize.Y - 1)),
    };
    SetConsoleCursorPosition(m_handle, xy);
}

//------------------------------------------------------------------------------
void win_screen_buffer::insert_chars(int count)
{
    if (count <= 0)
        return;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(m_handle, &csbi);

    SMALL_RECT rect;
    rect.Left = csbi.dwCursorPosition.X;
    rect.Right = csbi.dwSize.X;
    rect.Top = rect.Bottom = csbi.dwCursorPosition.Y;

    CHAR_INFO fill;
    fill.Char.AsciiChar = ' ';
    fill.Attributes = csbi.wAttributes;

    csbi.dwCursorPosition.X += count;

    ScrollConsoleScreenBuffer(m_handle, &rect, NULL, csbi.dwCursorPosition, &fill);
}

//------------------------------------------------------------------------------
void win_screen_buffer::delete_chars(int count)
{
    if (count <= 0)
        return;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(m_handle, &csbi);

    SMALL_RECT rect;
    rect.Left = csbi.dwCursorPosition.X + count;
    rect.Right = csbi.dwSize.X - 1;
    rect.Top = rect.Bottom = csbi.dwCursorPosition.Y;

    CHAR_INFO fill;
    fill.Char.AsciiChar = ' ';
    fill.Attributes = csbi.wAttributes;

    ScrollConsoleScreenBuffer(m_handle, &rect, NULL, csbi.dwCursorPosition, &fill);

    int chars_moved = rect.Right - rect.Left + 1;
    if (chars_moved < count)
    {
        COORD xy = csbi.dwCursorPosition;
        xy.X += chars_moved;

        count -= chars_moved;

        DWORD written;
        FillConsoleOutputCharacterW(m_handle, ' ', count, xy, &written);
        FillConsoleOutputAttribute(m_handle, csbi.wAttributes, count, xy, &written);
    }
}

//------------------------------------------------------------------------------
void win_screen_buffer::set_attributes(attributes attr)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(m_handle, &csbi);

    int out_attr = csbi.wAttributes & attr_mask_all;

    // Note to self; lookup table is probably much faster.
    auto swizzle = [] (int rgbi) {
        int b_r_ = ((rgbi & 0x01) << 2) | !!(rgbi & 0x04);
        return (rgbi & 0x0a) | b_r_;
    };

    // Map RGB/XTerm256 colors
    if (!get_nearest_color(attr))
        return;

    // Bold
    if (auto bold_attr = attr.get_bold())
        m_bold = !!(bold_attr.value);

    // Underline
    if (auto underline = attr.get_underline())
    {
        if (underline.value)
            out_attr |= attr_mask_underline;
        else
            out_attr &= ~attr_mask_underline;
    }

    // Foreground color
    bool bold = m_bold;
    if (auto fg = attr.get_fg())
    {
        int value = fg.is_default ? m_default_attr : swizzle(fg->value);
        value &= attr_mask_fg;
        out_attr = (out_attr & ~attr_mask_fg) | value;
        bold |= (value > 7);
    }
    else
        bold |= (out_attr & attr_mask_bold) != 0;

    if (bold)
        out_attr |= attr_mask_bold;
    else
        out_attr &= ~attr_mask_bold;

    // Background color
    if (auto bg = attr.get_bg())
    {
        int value = bg.is_default ? m_default_attr : (swizzle(bg->value) << 4);
        out_attr = (out_attr & ~attr_mask_bg) | (value & attr_mask_bg);
    }

    // Reverse video
    if (auto rev = attr.get_reverse())
    {
        if (rev.value)
        {
            int fg = (out_attr & ~attr_mask_bg);
            int bg = (out_attr & attr_mask_bg);
            out_attr = (fg << 4) | (bg >> 4);
        }
    }

    out_attr |= csbi.wAttributes & ~attr_mask_all;
    SetConsoleTextAttribute(m_handle, short(out_attr));
}

//------------------------------------------------------------------------------
static bool get_nearest_color(void* handle, const unsigned char (&rgb)[3], unsigned char& attr)
{
    static HMODULE hmod = GetModuleHandle("kernel32.dll");
    static FARPROC proc = GetProcAddress(hmod, "GetConsoleScreenBufferInfoEx");
    typedef BOOL (WINAPI* GCSBIEx)(HANDLE, PCONSOLE_SCREEN_BUFFER_INFOEX);

    if (!proc)
        return false;

    CONSOLE_SCREEN_BUFFER_INFOEX infoex = { sizeof(infoex) };
    if (!GCSBIEx(proc)(handle, &infoex))
        return false;

    cie::lab target(RGB(rgb[0], rgb[1], rgb[2]));
    float best_deltaE = 0;
    int best_idx = -1;

    for (int i = sizeof_array(infoex.ColorTable); i--;)
    {
        cie::lab candidate(infoex.ColorTable[i]);
        float deltaE = cie::deltaE(target, candidate);
        if (best_idx < 0 || best_deltaE > deltaE)
        {
            best_deltaE = deltaE;
            best_idx = i;
        }
    }

    if (best_idx < 0)
        return false;

    static const int dos_to_ansi_order[] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    attr = (best_idx & 0x08) + dos_to_ansi_order[best_idx & 0x07];
    return true;
}

//------------------------------------------------------------------------------
bool win_screen_buffer::get_nearest_color(attributes& attr)
{
    const attributes::color fg = attr.get_fg().value;
    const attributes::color bg = attr.get_bg().value;
    if (fg.is_rgb)
    {
        unsigned char val;
        unsigned char rgb[3];
        fg.as_888(rgb);
        if (!::get_nearest_color(m_handle, rgb, val))
            return false;
        attr.set_fg(val);
    }
    if (bg.is_rgb)
    {
        unsigned char val;
        unsigned char rgb[3];
        bg.as_888(rgb);
        if (!::get_nearest_color(m_handle, rgb, val))
            return false;
        attr.set_bg(val);
    }
    return true;
}
