/**
 * @file http_llms.h
 *
 * An `/llms.txt`: the emerging convention for a Markdown document that guides a large language model to
 * a site's key content — an H1 name, a blockquote summary, then sections of `- [title](url): note`
 * links. Built from sections a program supplies, with a strict preset that states restricted use up
 * front for a site that does not want its content used for training.
 *
 * ```
 * nya_http_llms_build         a name, a summary and sections -> the Markdown document
 * nya_http_llms_strict        the same, with a restricted-use notice at the top
 * nya_http_llms_mount         builds and serves it at GET /llms.txt as text/markdown
 * nya_http_llms_mount_strict  serves the strict document at GET /llms.txt
 * ```
 *
 * ```c
 * const NYA_HttpLlmsLink docs[] = {
 *     { .title = "API reference", .url = "https://example.com/api", .note = "every endpoint" },
 * };
 * const NYA_HttpLlmsSection sections[] = { { .heading = "Docs", .links = docs, .link_count = 1 } };
 * NYA_EXPECT(nya_http_llms_mount_strict((NYA_HttpLlmsConfig){
 *     .name = "Example", .summary = "An example site.", .sections = sections, .section_count = 1 }));
 * ```
 *
 * ── the escaping, and the strict notice ──
 *
 * A link title is escaped for Markdown so a `]` in it cannot close the link early, and every other field
 * is checked for control bytes — a newline in a heading or a note would restructure the document — and
 * refuses the build if it carries one. A link's URL is gated by nya_http_doc_url_is_web, and additionally
 * refused if it carries a parenthesis, which `(url)` has no way to hold.
 *
 * The strict preset writes a blockquote before the summary that says the content is for reading only and
 * not for training, fine-tuning or bulk collection. It is a request, not an enforcement — nothing here
 * can make a crawler honour it — but it is the plain statement of intent the convention is for, in the
 * one place a model reading the file will see first.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

// CONSTANTS

#define NYA_HTTP_LLMS_PATH "/llms.txt"

/** Sections one document here holds, and links within one section. Loop ceilings under the shared byte bound. */
#define NYA_HTTP_LLMS_MAX_SECTIONS 64
#define NYA_HTTP_LLMS_MAX_LINKS    256

/**
 * The restricted-use notice the strict preset writes near the top: the plain statement that the linked
 * content is for reading, not for training a model on. A `#define` so a test can assert it is present and
 * a caller can recognise it.
 * */
#define NYA_HTTP_LLMS_STRICT_NOTICE \
    "This site's content is provided for reading by people and permitted crawlers only. " \
    "It may not be used to train, fine-tune or evaluate machine-learning models, and automated bulk collection is not permitted."

// TYPES

typedef struct NYA_HttpLlmsLink    NYA_HttpLlmsLink;
typedef struct NYA_HttpLlmsSection NYA_HttpLlmsSection;
typedef struct NYA_HttpLlmsConfig  NYA_HttpLlmsConfig;

/** One `- [title](url): note` link. `title` and `url` are required; `note` is optional. */
struct NYA_HttpLlmsLink {
    NYA_ConstCString title; /**< Escaped for Markdown. Required. */
    NYA_ConstCString url;   /**< Required, http or https, and without a parenthesis. */
    NYA_ConstCString note;  /**< A short gloss after the link. Optional. */
};

/** One `## heading` and the links under it. */
struct NYA_HttpLlmsSection {
    NYA_ConstCString        heading; /**< Required. */
    const NYA_HttpLlmsLink* links;
    u32                     link_count;
};

/** The document: an H1 name, an optional blockquote summary and notes, and the sections. */
struct NYA_HttpLlmsConfig {
    NYA_ConstCString name;    /**< The site name, the H1. Required. */
    NYA_ConstCString summary; /**< A one-line blockquote under it. Optional. */
    NYA_ConstCString notes;   /**< A paragraph after the summary. Optional. */

    const NYA_HttpLlmsSection* sections;
    u32                        section_count;
};

// FUNCTIONS

/**
 * Builds the Markdown document into `out`, allocated from `arena`.
 *
 * NYA_ERROR_INVALID_ARGUMENT for a missing name, more than the section or link ceilings, a section with
 * no heading, a link with no title, a control byte in any field, or a URL that is not http/https or
 * carries a parenthesis; NYA_ERROR_OUT_OF_MEMORY on overflow, with nothing emitted.
 * */
NYA_API NYA_Error nya_http_llms_build(NYA_Arena* arena, NYA_HttpLlmsConfig config, OUT NYA_ConstCString* out) __attr_no_discard;

/** Builds the document with the restricted-use notice at the top. Same refusals as the plain build. */
NYA_API NYA_Error nya_http_llms_strict(NYA_Arena* arena, NYA_HttpLlmsConfig config, OUT NYA_ConstCString* out) __attr_no_discard;

/** Builds from `config` and serves it at GET /llms.txt as text/markdown. Merge nya_http_doc_router() to bring it online. */
NYA_API NYA_Error nya_http_llms_mount(NYA_HttpLlmsConfig config);

/** Builds the strict document and serves it at GET /llms.txt as text/markdown. */
NYA_API NYA_Error nya_http_llms_mount_strict(NYA_HttpLlmsConfig config);
