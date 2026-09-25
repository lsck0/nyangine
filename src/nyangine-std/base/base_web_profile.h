/**
 * @file base_web_profile.h
 *
 * The one line every server-only header carries, and the whole of the compile-time half of the
 * "Model, SO, DTO" gate. A `*_model.h` or `*_so.h` includes this first; the `web` profile builds with
 * `-DNYA_WEB_PROFILE`, and this then refuses to compile, so the server's storage layout — and any
 * secret in it — cannot be pulled into a wasm translation unit even by mistake.
 *
 * The client is sent the DTOs and nothing else. A `*_dto.h` therefore never includes this: it is the
 * one shape the web profile compiles, and including this would make it refuse itself. `./build check`
 * enforces both directions — every model/so header carries this include, no dto header includes a
 * model/so header — so a header that forgets the guard is a finding rather than a silent hole. See
 * TODO.md's "Model, SO, DTO" and _lint_rule_web_profile in src/build/lint.c.
 * */
#pragma once

#if defined(NYA_WEB_PROFILE)
#error "a server-only header (a *_model.h or *_so.h) was included in the web profile; the web client compiles *_dto.h only. Include the resource's *_dto.h instead, or convert to the DTO before it crosses the wire."
#endif
