"""Guest functions replaced by native runtime code.

For each address here the generated body is emitted as f_XXXXXXXX_guest and
f_XXXXXXXX itself must be defined by the runtime; every call and the
function table then reach the native version, which may still call the
generated body.
"""

OVERRIDES = {
    # Microthread scheduler: these switch stacks by restoring another
    # context's r1/lr, which generated code can't follow (runtime/ppu/fibers.cpp).
    0x72bb4: 'microthread yield (save current, pick next, restore it)',
    0x72a9c: 'microthread context restore (switch to a context, never returns)',
    # Native pick: the guest version spins without pause until a microthread is runnable.
    0x731f8: 'microthread pick (next runnable context; backs off while none is ready)',
    # Debugging hook (runtime/diag/debug_hooks.cpp): logs reads that land in HELSARI_HOOK_WATCH.
    0xa5ed6c: 'buffered asset stream read (stream, dst, size)',
    # Debugging hook (runtime/diag/debug_hooks.cpp): the scripts' trace and print
    # functions, empty in this build, are logged.
    0x3eea80: 'script trace / print (L): does nothing in the shipped game',
    0x77f90: 'script SwitchToGameModule (L): calls a handler stored in the module object',
    # GRAPHICS section of the options menu (port/pc_options.cpp).
    0x4894e0: 'menu URL dispatcher (this, url): takes the clicks of our rows',
    0x487fd0: 'calls a script function: adds our section when a page opens',
    # 1:1 mouse look (port/mouse.cpp).
    0x81f10: 'look input (this, outY, outX, settings): adds the mouse to the stick value',
    0x66f688: 'view data from a camera struct, per rendered frame: the view follows the mouse here',
    # PC prompts (port/keybinds.cpp).
    0x2dc278: 'the <buttonimage> text tag: shows the bound key instead of the PS3 icon',
    # Lua type errors, logged before the game reports them (runtime/diag/lua_errors.cpp).
    0x3f7ba8: 'Lua type error (L, value, operation)',
}
