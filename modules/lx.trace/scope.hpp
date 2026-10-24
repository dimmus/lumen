// `LX_TRACE_SCOPE(cat, name)`: an `lx::trace::scoped_span` for the rest of the enclosing
// block. A macro, so it lives in a header: a named module exports no macros, which is why
// `import lx.trace;` gives the class and not this. Include after `import lx.trace;`.
//
// `__COUNTER__` through a two-level paste makes every expansion a distinct variable, so two
// scopes in one block are fine (`##__LINE__` pasted the token `__LINE__` itself).
#pragma once

#define LX_TRACE_PASTE_IMPL(a, b) a##b
#define LX_TRACE_PASTE(a, b) LX_TRACE_PASTE_IMPL(a, b)
#define LX_TRACE_SCOPE(cat, name) \
    ::lx::trace::scoped_span LX_TRACE_PASTE(_lx_span_, __COUNTER__){cat, name}
