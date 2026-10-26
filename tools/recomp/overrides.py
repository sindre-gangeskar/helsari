"""Guest functions replaced by native runtime code.

For each address here the generated body is emitted as f_XXXXXXXX_guest and
f_XXXXXXXX itself must be defined by the runtime; every call and the
function table then reach the native version, which may still call the
generated body.
"""

OVERRIDES = {
    # Microthread scheduler: these switch stacks by restoring another
    # context's r1/lr, which generated code can't follow (runtime/src/fibers.cpp).
    0x7203c: 'microthread yield (save current, pick next, restore it)',
    0x71f24: 'microthread context restore (switch to a context, never returns)',
}
