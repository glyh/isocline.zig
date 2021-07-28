/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/

#include <string.h>
#include "common.h"
#include "term.h"

//-------------------------------------------------------------
// Syntax highlighting
//-------------------------------------------------------------

// text attributes.
// (zero values keep the current attribute)
typedef struct attr_s {
  int8_t     underline;       // -1 = off, 0 = keep as is, 1 = on
  int8_t     reverse;         // -1 = off, 0 = keep as is, 1 = on
  rp_color_t color;           
  rp_color_t bgcolor;
} attr_t;

static const attr_t attr_zero    = { 0, 0, RP_COLOR_NONE, RP_COLOR_NONE };
static const attr_t attr_default = { -1, -1, RP_COLOR_DEFAULT, RP_COLOR_DEFAULT };

struct rp_highlight_env_s {
  attr_t*  attrs;
  ssize_t  attr_capacity;  
  ssize_t  attr_len;
  attr_t   cur_attr;
  alloc_t* mem;
};


//-------------------------------------------------------------
// Private interface
//-------------------------------------------------------------

rp_private rp_highlight_env_t* highlight_new( alloc_t* mem ) {
  rp_highlight_env_t* henv = mem_zalloc_tp( mem, rp_highlight_env_t );
  if (henv == NULL) return NULL;
  henv->mem = mem;
  return henv;
}

rp_private void highlight_free( rp_highlight_env_t* henv ) {
  if (henv == NULL) return;
  mem_free( henv->mem, henv->attrs );
  mem_free( henv->mem, henv );
}

static bool highlight_ensure_extra( rp_highlight_env_t* henv, ssize_t extra ) {
  if (henv==NULL) return false;
  ssize_t needed = henv->attr_len + extra;
  if (henv->attr_capacity < needed) {
    ssize_t capacity = (henv->attr_capacity <= 0 ? 128 : 2*henv->attr_capacity);
    if (capacity < needed) { capacity = needed; }
    attr_t* newattrs = mem_realloc_tp( henv->mem, attr_t, henv->attrs, capacity );
    if (newattrs == NULL) return false;
    henv->attrs = newattrs;
    henv->attr_capacity = capacity;
  }
  assert(henv->attr_capacity >= needed);
  return true;
}

rp_private bool highlight_insert_at( rp_highlight_env_t* henv, ssize_t pos, ssize_t len, rp_color_t color ) {
  if (henv == NULL) return false;
  if (pos < 0 || pos > henv->attr_len) return false;
  if (!highlight_ensure_extra(henv,len)) return false;
  rp_memmove(henv->attrs + pos + len, henv->attrs + pos, ssizeof(attr_t)*(henv->attr_len - pos));
  henv->attr_len += len;
  attr_t attr = attr_default;
  attr.color = color;
  for (ssize_t i = 0; i < len; i++) {
    henv->attrs[pos + i] = attr;
  }
  return true;
}

rp_private void highlight_clear( rp_highlight_env_t* henv ) {
  if (henv == NULL) return;
  henv->attr_len = 0;
  henv->cur_attr = attr_default;
}

static void highlight_fillout( rp_highlight_env_t* henv ) {
  if (henv == NULL) return;
  attr_t attr = attr_default;
  for( ssize_t i = 0; i < henv->attr_len; i++ ) {
    attr_t* cur = &henv->attrs[i];
    // propagate attribute
    if (cur->color     == 0) { cur->color = attr.color; }
    if (cur->bgcolor   == 0) { cur->bgcolor = attr.bgcolor; }
    if (cur->underline == 0) { cur->underline = attr.underline; }
    if (cur->reverse   == 0) { cur->reverse = attr.reverse; }
    attr = *cur;
  }
}

rp_private bool highlight_init( rp_highlight_env_t* henv, const char* s, rp_highlight_fun_t* highlighter ) {
  if (henv == NULL) return false;
  highlight_clear(henv);
  const ssize_t len = rp_strlen(s);
  if (len > 0) {
    if (!highlight_ensure_extra(henv,len)) return false;
    henv->attr_len = len;
    rp_memset(henv->attrs, 0, henv->attr_len * ssizeof(attr_t));
    if (highlighter != NULL) {
      (*highlighter)( henv, s );
    }
  }
  highlight_fillout(henv);
  return true;
}

//-------------------------------------------------------------
// Writing to the terminal
//-------------------------------------------------------------

static void term_update_attr(term_t* term, attr_t term_attr, attr_t new_attr, attr_t* cur_attr) {
  // update current attribute
  if (new_attr.color != 0 && cur_attr->color != new_attr.color) {
    cur_attr->color = new_attr.color;
  }
  if (new_attr.bgcolor != 0 && cur_attr->bgcolor != new_attr.bgcolor) {
    cur_attr->bgcolor = new_attr.bgcolor;
  }
  if (new_attr.reverse != 0 && cur_attr->reverse != new_attr.reverse) {
    cur_attr->reverse = new_attr.reverse;
  }
  if (new_attr.underline != 0 && cur_attr->underline != new_attr.underline) {
    cur_attr->underline = new_attr.underline;
  }
  // and write out escape sequences for any changes
  if (term_attr.color != cur_attr->color) {
    term_color(term, cur_attr->color);
  }
  if (term_attr.color != cur_attr->bgcolor) {
    term_bgcolor(term, cur_attr->bgcolor);
  }
  if (term_attr.underline != cur_attr->underline) {
    term_underline(term, cur_attr->underline > 0);
  }
  if (term_attr.reverse != cur_attr->reverse) {
    term_reverse(term, cur_attr->reverse > 0);
  }
}

rp_private void highlight_term_write( rp_highlight_env_t* henv, term_t* term, const char* s, ssize_t start, ssize_t len ) 
{
  if (henv == NULL || henv->attr_len <= 0) {
    term_write_n( term, s + start, len );    
  }
  else {
    attr_t term_attr = attr_zero;  // = unknown  
    for (ssize_t i = start; i < start + len; i++) {
      char c = s[i];
      if (i >= 0 && i < henv->attr_len && !utf8_is_cont((uint8_t)c)) {
        term_update_attr(term, term_attr, henv->attrs[i], &henv->cur_attr);
        term_attr = henv->cur_attr;
      }
      term_write_char(term, c);
    }    
  }
  term_attr_reset(term);
}


//-------------------------------------------------------------
// Client interface
//-------------------------------------------------------------

// Set the color of characters starting at position `pos` to `color`.
rp_public void rp_highlight_color(rp_highlight_env_t* henv, long pos, rp_color_t color ) {
  if (henv==NULL) return;
  if (pos < 0 || pos >= henv->attr_len) return;
  henv->attrs[pos].color = color;
}

// Set the background color of characters starting at position `pos` to `bgcolor`.
rp_public void rp_highlight_bgcolor(rp_highlight_env_t* henv, long pos, rp_color_t bgcolor) {
  if (henv==NULL) return;
  if (pos < 0 || pos >= henv->attr_len) return;
  henv->attrs[pos].bgcolor = bgcolor;
}

// Enable/Disable underlining for characters starting at position `pos`.
rp_public void rp_highlight_underline(rp_highlight_env_t* henv, long pos, bool enable ) {
  if (henv==NULL) return;
  if (pos < 0 || pos >= henv->attr_len) return;
  henv->attrs[pos].underline = (enable ? 1 : -1);
}

// Enable/Disable reverse video for characters starting at position `pos`.
rp_public void rp_highlight_reverse(rp_highlight_env_t* henv, long pos, bool enable) {
  if (henv==NULL) return;
  if (pos < 0 || pos >= henv->attr_len) return;
  henv->attrs[pos].underline = (enable ? 1 : -1);
}