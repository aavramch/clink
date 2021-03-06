// Copyright (c) 2015 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#pragma once

#include <functional>

struct lua_State;

//------------------------------------------------------------------------------
class lua_state
{
public:
                    lua_state();
                    ~lua_state();
    void            initialise();
    void            shutdown();
    bool            do_string(const char* string, int length=-1);
    bool            do_file(const char* path);
    lua_State*      get_state() const;

    static int      pcall(lua_State* L, int nargs, int nresults);
    int             pcall(int nargs, int nresults) { return pcall(m_state, nargs, nresults); }

    bool            send_event(const char* event_name, int nargs=0);
    bool            send_event_cancelable(const char* event_name, int nargs=0);

#ifdef DEBUG
    void            dump_stack(int pos);
#endif

private:
    bool            send_event_internal(const char* event_name, const char* event_mechanism, int nargs=0, int nret=0);
    lua_State*      m_state;
};

//------------------------------------------------------------------------------
inline lua_State* lua_state::get_state() const
{
    return m_state;
}

//------------------------------------------------------------------------------
// Dumps from pos to top of stack (use negative pos for relative position, use
// positive pos for absolute position, or use 0 for entire stack).
#ifdef DEBUG
void dump_lua_stack(lua_State* state, int pos);
#endif
