ChrisAnt Plans

<br/>

# RELEASE

- Investigate all usage of `putenv` since it doesn't write back to the process's environment block.  Consider updating `getenv` to `os::get_env`, and `putenv` to `os::set_env`.

## Issues
- The match pipeline should not fire on pressing **Enter** after `exit`.
- When `convert-meta` is off, then when binding `\M-h` (etc) the key name gets interpreted differently than Clink expects.  Does this affect the `inputrc` files at all, or is it only an issue inside Clink's native code?

## Cmder, Powerline
- Merge my powerline changes with cmder and cmder-powerline-prompt.
- Port Cmder to v1.x -- will require help from Cmder and/or ConEmu teams.  There are a lot of hard-coded expectations about Clink (web site address, terminal input mode, DLL names, VirtualAlloc patterns, and many other things).

<br/>
<br/>

# IMPROVEMENTS

## High Priority
- Allow binding keys to Lua scripts.
  - Add a new `ISUSER` custom binding type to Readline, which calls a global callback and passes it the binding string (like for `ISMACR`)?  The global callback can then look up the Lua function name and pass it a `line_state` from `collect_words(false/*stop_at_cursor*/)`.
    - But then how to make the inputrc file continue to be compatible for sharing with other terminal implementations?
    - Or just define some special prefix for macro output text, and have a macro hook function to allow the host to intercept and handle macros.
  - Provide API for scrolling.
  - Provide API for accessing the screen buffer.
    - Find text; return line, which can be used with the scrolling API?
    - Find attributes; return line, which can be used with the scrolling API?
    - Retrieve text and/or attributes?
    - Maybe have a way to have floating windows mark corners of a selection region?
    - Maybe have a way to generate HTML from text in a selection region?
  - Provide API for interacting with the Readline buffer.
  - Provide API to show a popup list?  But make it fail if used from outside a Readline command.
  - Provide API to show an input box?  But make it fail if used from outside a Readline command.

## Medium Priority
- Add a hook function for inserting matches.
  - The insertion hook can avoid appending a space when inserting a flag/arg that ends in `:` or `=`.
  - The insertion hook can deal with path normalisation, e.g. to clean up input like "\wbin\\\\cli" when using `complete` and `menu-complete`.
  - And address the sorting problem, and then the match_type stuff can be removed from Readline itself (though Chet may want its performance benefits).
  - And THEN individual matches can have arbitrary values associated -- color, append char, or any per-match data that's desired.
- Add a configuration setting for whether `menu-complete` wraps around.
- Complete "%ENVVAR%\*" by internally expanding ENVVAR for collecting matches, but not expanding it in the editing line.

## Low Priority
- Add commands that behave like **F7** and **F8** from CMD (like `history-search-backward` without wrapping around?).
- Add terminal sequences for **Ctrl+Shift+Letter** and **Ctrl+Punctuation** and etc (see https://invisible-island.net/xterm/modified-keys.html).
- Add a `history.dupe_mode` that behaves like 4Dos/4NT/Take Command from JPSoft:  **Up**/**Down** then **Enter** remembers the history position so that **Enter**, **Down**, **Enter**, **Down**, **Enter**, etc can be used to replay a series of commands.
- Symlink support (displaying matches, and whether to append a path separator).

## Clink-Completions
- Update clink-completions to have better 0.4.9 implementations, and also to conditionally use the new API when available.

<br/>
<br/>

# MAJOR WORK ITEMS

- **Coloring arguments and flags while editing (according to Lua argmatchers)**
  - Replace `rl_redisplay_function` and use the word classifications to apply colors.
  - OR maintain color-display buffers separate from the content-display buffers, and make update_line consult the color-display buffers to color the output.
- **CUA Selection.**
- **Make the match pipeline async.**
  - Spin up completion at the same moment it currently does, but make it async.
  - Only block consumers when they try to access results, if not yet complete.  Also have "try-" access that accesses if available but doesn't block (e.g. will be needed for coloring arguments while editing).
  - This will solve the UNC performance problem and also make associated pauses break-able.
  - Lua and Lua scripts will need multi-threading support.

<br/>
<br/>

# INVESTIGATE

**Documentation**
- Use `npm` to run `highlight.js` at compile time?

**Popup Lists**
- Ability to delete, rearrange, and edit popup list items?
- Show the current incremental search string somewhere?
- Completions:
  - What to do about completion colors?
  - Make it owner draw and add text like "dir", "alias", etc?
  - Add stat chars when so configured?

**Key Binding**
- Try to make unbound keys like **Shift-Left** tell conhost that they haven't been handled, so conhost can do its fancy CUA marking.
  - Mysteriously, holding down a bound key like **Ctrl+Up** or **Ctrl+A** was sometimes letting conhost periodically intercept some of the keypresses!  That shouldn't be possible, but maybe there's a way to deterministically cause that behavior?
  - **Ctrl+M** to activate marking mode.
  - Shouldn't be possible, but at one point **Ctrl+A** was sometimes able to get interpreted as Select All, and **Shift+Up** was sometimes able to get interpreted as Select Line Up.  That had to have been a bug in how clink was managing SetConsoleMode, but maybe there's a way to exploit that for good?
    - Maybe hook some API and call original ReadConsoleW, and feed it input somehow to trick the console host into doing its thing?

**Prompt Filtering**
- Async command prompt updating as a way to solve the delay in git repos.

**Marking**
- Marking mode in-app similar to my other shell project?  It's a kludge, but it copies with HTML formatting (and even uses the color scheme).

**Miscellaneous**
- Changing terminal width makes 0.4.8 slowly "walk up the screen".  Changing terminal width works in master, except when the cursor position itself is affected.
- Is it a problem that `update_internal()` gets called once per char in a key sequence?  Maybe it should only happen after a key that finishes a key binding?
- Should only fold path separators in pathish matches.
- Git stashes like `stash@{2}` stop completing once you type the `{`.
- How to reasonably support normal completion coloring with `ondisplaymatches` match display filtering?
- Allow to search the console output (not command history) with a RegExp [#166](https://github.com/mridgers/clink/issues/166).  _[Unclear how that would work.  Would it scroll the console?  How would it highlight matches, etc, since that's really something the console host would need to do?  I think this needs to be implemented by the console host, e.g. conhost or ConEmu or Terminal, etc.]_
- [#20](https://github.com/chrisant996/clink/issues/20) Cmd gets unresponsive after "set /p" command.  _[Seems to mostly work, though `set /p FOO=""` doesn't prompt for input.]_
- Include `wildmatch()` and an `fnmatch()` wrapper for it.  But should first update it to support UTF8.
- `LOG()` certain important failure information inside Detours.

<br/>
<br/>

# MAINTENANCE

- Contact Readline owner, start conversation about possible next steps.

## Remove match type changes from Readline?
- Displaying matches was slow because Readline writes everything one byte at a time, which incurs significant processing overhead across several layers.
- Adding a new `rl_completion_display_matches_func` and `display_matches()` resolved the performance satisfactorily.
- It's now potentially possible to revert the changes to feed Readline match type information.  It's only used when displaying matches, and when inserting a match to decide whether to append a path separator.  Maybe just add a callback for inserting.
- But it's not straightforward since Readline sorts the matches, making it difficult to translate the sorted index to the unsorted list held by `matches_impl`.

<br/>
<br/>

# BACKLOG

- [#542](https://github.com/mridgers/clink/issues/542) VS Code not capturing std output
- [#486](https://github.com/mridgers/clink/issues/486) **Ctrl+C** doesn't always work properly _[might be the auto-answer prompt setting]_
- A way to disable/enable clink once injected.
- [#532](https://github.com/mridgers/clink/issues/532) paste newlines, run as separate lines
  - It's pretty risky to just paste-and-go.
  - Maybe add an option to convert newlines into "&" instead?
  - Or maybe let readline do multiline editing and accept them all as a batch on **Enter**?

<br/>
<br/>

# APPENDICES

## Manual Test Verifications
- <kbd>Alt</kbd>+<kbd>Up</kbd> to scroll up a line.
- `git add ` in Cmder.
- `git checkout `<kbd>Alt</kbd>+<kbd>=</kbd> in Cmder.

## Known Issues
- Perturbed PROMPT envvar is visible in child processes (e.g. piped shell in various file editors).
- [#531](https://github.com/mridgers/clink/issues/531) AV detects a trojan on download _[This is likely because of the use of CreateRemoteThread and/or hooking OS APIs.  There might be a way to obfuscate the fact that clink uses those, but ultimately this is kind of an inherent problem.  Getting the binaries digitally signed might be the most effective solution, but that's financially expensive.]_
- Readline's incremental display updates plus its reliance on ANSI escape codes for cursor positioning seem to make it not able to properly support editing within a line containing surrogate pairs.  I would say that it's a Readline issue, except that git-bash seems to be a bit better at it than Clink is, so maybe Clink isn't hosting Readline correctly.

## Mystery
- [#480](https://github.com/mridgers/clink/issues/480) Things don't work right when clink is in a path with spaces _[I'm not able to reproduce the problem, so dropping it off the radar for now.]_
- Windows 10.0.19042.630 seems to have problems when using WriteConsoleW with ANSI escape codes in a powerline prompt in a git repo.  But Windows 10.0.19041.630 doesn't.

---
Chris Antos - sparrowhawk996@gmail.com
