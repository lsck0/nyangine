#!/bin/sh
#
# commit-msg-lint.sh — the one core behind the commit-msg git hook and `./build commit-check`.
#
# It reads a single commit message, or every message in a git revision range, and holds each to the
# rules this repository commits under:
#
#   * the subject is a conventional commit — `type(scope): subject`, an optional `!` before the colon
#     for a breaking change — where the type is one this project uses;
#   * the subject is not absurdly long (a hard ceiling that catches a body pasted onto the first line,
#     with a softer nudge toward the ~72 the changelog reads best at);
#   * nothing in the message is an AI-attribution trailer — no `Co-Authored-By: … Claude`, no
#     "Generated with", no robot emoji. This repo's history was rewritten once to strip exactly these,
#     and the point of the gate is that it never has to be again.
#
# Two entry points share this core so the rules live in one place: the `commit-msg` hook next to this
# file runs it on the message a commit is about to record, and `./build commit-check` runs it over a
# range so CI can hold a whole pull request to the same bar. Both invoke this script; neither restates
# a rule.
#
# POSIX sh and git are the only dependencies — no node, no python — so a bare checkout can enforce it.
#
# Usage:
#   commit-msg-lint.sh <file>            lint the message in <file>          (what the hook passes)
#   commit-msg-lint.sh -f <file>         the same, said explicitly
#   commit-msg-lint.sh -r <range>        lint every commit in a git <range>, e.g. origin/master..HEAD
#   commit-msg-lint.sh                   lint the message on stdin
#
# Exit status is zero when every message passed and non-zero when one did not. Warnings never fail.

set -u

# ── Tunables ─────────────────────────────────────────────────────────────────────────────────────
#
# The soft limit is a nudge, not a gate: this codebase writes deliberately literate subjects, and its
# own recent history runs past 72 often enough that failing on it would reject the committed style.
# The hard limit is the real gate — it exists to catch a body accidentally left on the subject line,
# not to enforce brevity. Both are overridable from the environment.
SUBJECT_WARN=${NYA_COMMIT_SUBJECT_WARN:-72}
SUBJECT_MAX=${NYA_COMMIT_SUBJECT_MAX:-200}

# The conventional-commit types this repository uses: the set the changelog generator groups under
# (changelog.c) plus chore, style and revert, which never reach the changelog but are still valid.
TYPES='feat|fix|docs|refactor|perf|test|build|ci|chore|style|revert|example|bench|tune'

# ── One message ───────────────────────────────────────────────────────────────────────────────────

# lint_one <raw-message> <strip> <label>
#
# <strip> is 1 for a message straight from git's editor, which still carries comment lines and, under
# `commit -v`, the diff below a scissors line; git removes those after this hook runs, so we remove
# them here too before reading the subject. It is 0 for a message already committed, which is clean.
# <label> names the message in any diagnostic. Returns 0 when the message passed, 1 when it did not.
lint_one() {
    raw=$1
    strip=$2
    label=$3

    if [ "$strip" = 1 ]; then
        # git's own default cleanup: drop everything from the scissors line down, then every remaining
        # comment line. The comment character is '#' unless core.commentChar was changed, which is rare
        # enough not to chase here.
        message=$(printf '%s\n' "$raw" | awk '/^#.*>8/ { exit } /^#/ { next } { print }')
    else
        message=$raw
    fi

    # The subject is the first non-blank line. An empty message is left for git to reject on its own
    # terms rather than blamed on the style rules.
    subject=$(printf '%s\n' "$message" | awk 'NF { print; exit }')
    if [ -z "$subject" ]; then
        return 0
    fi

    failed=0

    # A merge or a revert git wrote itself, and the autosquash markers rebase consumes, are transient
    # and never land as history — waving them through keeps the hook out of git's own way.
    case $subject in
        'Merge '* | 'Revert '* | 'fixup! '* | 'squash! '* | 'amend! '*)
            return 0
            ;;
    esac

    # The subject shape: a known type, an optional (scope) with no spaces or parens inside, an optional
    # ! for a breaking change, then ": " and a non-empty description.
    if ! printf '%s' "$subject" | grep -qE "^(${TYPES})(\([^)( ]+\))?!?: .+"; then
        _fail "$label" "subject is not a conventional commit."
        printf '    got:      %s\n' "$subject" >&2
        printf '    expected: type(scope): subject   (type is one of: %s)\n' "$(printf '%s' "$TYPES" | tr '|' ' ')" >&2
        failed=1
    fi

    # Length. ${#subject} is bytes, not characters; a subject is overwhelmingly ASCII, so the two agree,
    # and where a stray multibyte character makes them differ it only ever counts against a subject
    # already near the ceiling.
    length=${#subject}
    if [ "$length" -gt "$SUBJECT_MAX" ]; then
        _fail "$label" "subject is $length characters, over the hard limit of $SUBJECT_MAX."
        printf '    a subject this long is usually a body left on the first line.\n' >&2
        failed=1
    elif [ "$length" -gt "$SUBJECT_WARN" ]; then
        printf '%s: note: subject is %s characters; %s or fewer reads best in the changelog.\n' "$PROG" "$length" "$SUBJECT_WARN" >&2
    fi

    # AI attribution, anywhere in the message that will be recorded. The three shapes the rewrite
    # stripped: a Co-Authored-By trailer naming Claude or Anthropic, a "Generated with" line, and the
    # robot emoji.
    if printf '%s\n' "$message" | grep -qiE 'co-authored-by:.*(claude|anthropic)'; then
        _fail "$label" "carries a Co-Authored-By trailer naming an AI. Remove it."
        failed=1
    fi
    if printf '%s\n' "$message" | grep -qi 'generated with'; then
        _fail "$label" 'carries a "Generated with" attribution line. Remove it.'
        failed=1
    fi
    if printf '%s\n' "$message" | grep -qF '🤖'; then
        _fail "$label" 'carries the 🤖 attribution emoji. Remove it.'
        failed=1
    fi

    return "$failed"
}

# _fail <label> <reason> — one diagnostic line, to stderr so a capturing caller still sees it.
_fail() {
    printf '%s: %s %s\n' "$PROG" "$1" "$2" >&2
}

# ── Entry point ───────────────────────────────────────────────────────────────────────────────────

PROG=commit-msg-lint

mode=file
target=

case ${1:-} in
    -r | --range)
        mode=range
        target=${2:-}
        if [ -z "$target" ]; then
            printf '%s: -r needs a git revision range, e.g. origin/master..HEAD\n' "$PROG" >&2
            exit 2
        fi
        ;;
    -f | --file)
        mode=file
        target=${2:-}
        if [ -z "$target" ]; then
            printf '%s: -f needs a path to a commit message file\n' "$PROG" >&2
            exit 2
        fi
        ;;
    '')
        mode=stdin
        ;;
    *)
        mode=file
        target=$1
        ;;
esac

status=0

case $mode in
    stdin)
        raw=$(cat)
        lint_one "$raw" 1 'commit message' || status=1
        ;;
    file)
        if [ ! -r "$target" ]; then
            printf '%s: cannot read commit message file: %s\n' "$PROG" "$target" >&2
            exit 2
        fi
        raw=$(cat "$target")
        lint_one "$raw" 1 "$target" || status=1
        ;;
    range)
        revs=$(git rev-list "$target") || exit 2
        if [ -z "$revs" ]; then
            printf '%s: no commits in range %s; nothing to check.\n' "$PROG" "$target" >&2
            exit 0
        fi
        checked=0
        for rev in $revs; do
            raw=$(git log -1 --format=%B "$rev")
            short=$(printf '%s' "$rev" | cut -c1-9)
            lint_one "$raw" 0 "commit $short" || status=1
            checked=$((checked + 1))
        done
        if [ "$status" = 0 ]; then
            printf '%s: %s commit(s) in %s pass.\n' "$PROG" "$checked" "$target" >&2
        fi
        ;;
esac

exit "$status"
