/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <book_edit.h>
#include <string.h>

static const book_edit_entry_t *entry(const book_edit_model_t *model, uint32_t tag) {
    for (size_t i = 0; i < model->current.count; i++) {
        if (model->current.books[i].tag == tag) {
            return &model->current.books[i];
        }
    }
    return NULL;
}

bool book_edit_model_dirty(const book_edit_model_t *model) {
    return model->destination != 0 &&
           (strcmp(model->title, model->base_title) ||
            strcmp(model->contents, model->base_contents));
}

static void adopt(book_edit_model_t *model, const book_edit_snapshot_t *next, bool retain) {
    model->current = *next;
    model->pending = false;
    model->confirmation = BOOK_CONFIRM_NONE;
    model->destination = next->selected;
    strcpy(model->base_title, next->title);
    strcpy(model->base_contents, next->contents);
    if (!retain) {
        strcpy(model->title, next->title);
        strcpy(model->contents, next->contents);
    }
    if (!entry(model, model->source)) {
        model->source = next->count ? next->books[0].tag : 0;
    }
}

bool book_edit_model_receive(book_edit_model_t *model, const book_edit_snapshot_t *next) {
    if (next->result != BOOK_EDIT_OPEN &&
        (model->current.session == 0 || next->session != model->current.session)) {
        return false;
    }
    if (next->result == BOOK_EDIT_OPEN && book_edit_model_dirty(model)) {
        bool different = next->selected != model->destination;
        bool changed = strcmp(next->title, model->base_title) ||
                       strcmp(next->contents, model->base_contents);
        if (different || changed) {
            model->incoming = *next;
            /* The new OPEN has replaced the server's old session. No request
             * may use it until the player approves discard or rebase. */
            model->current.session = 0;
            model->pending = false;
            model->confirmation = different ? BOOK_CONFIRM_OPEN_DISCARD : BOOK_CONFIRM_OPEN_REBASE;
            return true;
        }
        adopt(model, next, true);
    } else if (next->result == BOOK_EDIT_ERROR) {
        /* Errors update inventory/ink, never the retained draft's saved base. */
        model->current = *next;
        model->pending = false;
        model->confirmation = BOOK_CONFIRM_NONE;
    } else {
        adopt(model, next, false);
    }
    return true;
}

void book_edit_model_resolve_open(book_edit_model_t *model, bool accept) {
    bool retain = model->confirmation == BOOK_CONFIRM_OPEN_REBASE;
    if (model->confirmation != BOOK_CONFIRM_OPEN_DISCARD && !retain) {
        return;
    }
    if (accept) {
        adopt(model, &model->incoming, retain);
    } else {
        model->current.session = 0;
        model->confirmation = BOOK_CONFIRM_NONE;
    }
    memset(&model->incoming, 0, sizeof(model->incoming));
}

void book_edit_model_close(book_edit_model_t *model) {
    model->current.session = 0;
    model->pending = false;
    model->confirmation = BOOK_CONFIRM_NONE;
    memset(&model->incoming, 0, sizeof(model->incoming));
}

void book_edit_model_cancel(book_edit_model_t *model) {
    memset(model, 0, sizeof(*model));
}

bool book_edit_model_can_submit(const book_edit_model_t *model, enum book_edit_action action,
                                uint32_t destination) {
    if (model->pending || !model->current.session) {
        return false;
    }
    if (action == BOOK_EDIT_CANCEL) {
        return true;
    }
    const book_edit_entry_t *dst = entry(model, destination);
    if (!dst || dst->finalized) {
        return false;
    }
    if (action == BOOK_EDIT_SELECT) {
        return (model->confirmation == BOOK_CONFIRM_SELECT &&
                destination == model->selection_target) ||
               (model->confirmation == BOOK_CONFIRM_NONE && !book_edit_model_dirty(model));
    }
    if (destination != model->destination || destination != model->current.selected) {
        return false;
    }
    if (action == BOOK_EDIT_COPY) {
        return model->confirmation == BOOK_CONFIRM_COPY &&
               destination == model->selection_target &&
               model->source == model->confirmed_source &&
               model->source != destination && entry(model, model->source);
    }
    if (action == BOOK_EDIT_SIGN) {
        return model->confirmation == BOOK_CONFIRM_SIGN &&
               destination == model->selection_target && !book_edit_model_dirty(model);
    }
    return action == BOOK_EDIT_SAVE && model->confirmation == BOOK_CONFIRM_NONE;
}

bool book_edit_model_confirm(book_edit_model_t *model, enum book_edit_action action,
                             uint32_t destination) {
    if ((action != BOOK_EDIT_COPY && action != BOOK_EDIT_SIGN && action != BOOK_EDIT_SELECT) ||
        model->pending || !model->current.session || model->confirmation != BOOK_CONFIRM_NONE) {
        return false;
    }
    enum book_edit_confirmation next = action == BOOK_EDIT_COPY ? BOOK_CONFIRM_COPY :
                                      action == BOOK_EDIT_SIGN ? BOOK_CONFIRM_SIGN : BOOK_CONFIRM_SELECT;
    model->confirmation = next;
    model->selection_target = destination;
    model->confirmed_source = model->source;
    if (!book_edit_model_can_submit(model, action, destination)) {
        model->confirmation = BOOK_CONFIRM_NONE;
        return false;
    }
    return true;
}
