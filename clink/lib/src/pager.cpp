// Copyright (c) 2020 Christopher Antos, Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include <assert.h>
#include "pager.h"
#include "binder.h"
#include "editor_module.h"
#include "line_buffer.h"
#include "line_state.h"

#include <core/base.h>
#include <core/settings.h>
#include <core/str_iter.h>
#include <terminal/printer.h>
#include <terminal/setting_colour.h>

setting_colour g_colour_interact(
    "colour.interact",
    "For user-interaction prompts",
    "Used when Clink displays text or prompts such as a pager's 'More?'.",
    setting_colour::value_light_magenta, setting_colour::value_bg_default);



//------------------------------------------------------------------------------
enum {
    bind_id_pager_page      = 30,
    bind_id_pager_halfpage,
    bind_id_pager_line,
    // TODO: bind_id_pager_help,
    bind_id_pager_stop,

    bind_id_catchall,
};



//------------------------------------------------------------------------------
pager::pager(input_dispatcher& dispatcher)
    : m_dispatcher(dispatcher)
{
}

//------------------------------------------------------------------------------
void pager::bind_input(binder& binder)
{
    m_pager_bind_group = binder.create_group("pager");
    binder.bind(m_pager_bind_group, " ", bind_id_pager_page);
    binder.bind(m_pager_bind_group, "\t", bind_id_pager_page);
    binder.bind(m_pager_bind_group, "\r", bind_id_pager_line);
    binder.bind(m_pager_bind_group, "d", bind_id_pager_halfpage);
    binder.bind(m_pager_bind_group, "D", bind_id_pager_halfpage);
    binder.bind(m_pager_bind_group, "q", bind_id_pager_stop);
    binder.bind(m_pager_bind_group, "Q", bind_id_pager_stop);
    binder.bind(m_pager_bind_group, "^C", bind_id_pager_stop); // ctrl-c
    binder.bind(m_pager_bind_group, "^D", bind_id_pager_stop); // ctrl-d
    binder.bind(m_pager_bind_group, "^[", bind_id_pager_stop); // esc

    binder.bind(m_pager_bind_group, "", bind_id_catchall);
}

//------------------------------------------------------------------------------
void pager::on_begin_line(const context& context)
{
}

//------------------------------------------------------------------------------
void pager::on_end_line()
{
}

//------------------------------------------------------------------------------
void pager::on_matches_changed(const context& context)
{
}

//------------------------------------------------------------------------------
void pager::on_input(const input& input, result& result, const context& context)
{
    switch (input.id)
    {
    case bind_id_pager_page:        start_pager(context, page); break;
    case bind_id_pager_halfpage:    start_pager(context, half_page); break;
    case bind_id_pager_line:        start_pager(context, line); break;
    case bind_id_pager_stop:        m_max = -1; break;
    }
}

//------------------------------------------------------------------------------
void pager::on_terminal_resize(int columns, int rows, const context& context)
{
}

//------------------------------------------------------------------------------
void pager::start_pager(const context& context, pager_amount amount)
{
    switch (amount)
    {
    case unlimited:     m_max = 0; break;
    case line:          m_max = 1; break;
    case half_page:     m_max = max<int>(context.printer.get_rows() / 2, 0); break;
    case page:          m_max = max<int>(context.printer.get_rows() - 2, 0); break;
    case first_page:    m_max = max<int>(context.printer.get_rows() - 1, 0); break;
    }
}

//------------------------------------------------------------------------------
bool pager::on_print_lines(const context& context, int lines)
{
    if (m_max < 0)
    {
        m_max = 0;
        return false;
    }

    if (m_max == 0)
        return true;

    m_max -= lines;
    if (m_max > 0)
        return true;
    m_max = 0;

    context.printer.print(g_colour_interact.get(), "-- More --");
    m_dispatcher.dispatch(m_pager_bind_group);

    // \x1b[1G is needed because win_terminal_out doesn't handle \r, and I don't
    // want to introduce the performance hit of intercepting \r except where
    // it's really needed, like here.
    context.printer.print("\x1b[1K\x1b[1G");
    return true;
}
