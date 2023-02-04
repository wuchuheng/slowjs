#include "parse.h"

#include "vm/mod.h"

/* XXX: improve speed with early bailout */
/* XXX: no longer works if regexps are present. Could use previous
   regexp parsing heuristics to handle most cases */
int js_parse_skip_parens_token(JSParseState *s, int *pbits,
                               BOOL no_line_terminator) {
  char state[256];
  size_t level = 0;
  JSParsePos pos;
  int last_tok, tok = TOK_EOF;
  int c, tok_len, bits = 0;

  /* protect from underflow */
  state[level++] = 0;

  js_parse_get_pos(s, &pos);
  last_tok = 0;
  for (;;) {
    switch (s->token.val) {
    case '(':
    case '[':
    case '{':
      if (level >= sizeof(state))
        goto done;
      state[level++] = s->token.val;
      break;
    case ')':
      if (state[--level] != '(')
        goto done;
      break;
    case ']':
      if (state[--level] != '[')
        goto done;
      break;
    case '}':
      c = state[--level];
      if (c == '`') {
        /* continue the parsing of the template */
        free_token(s, &s->token);
        /* Resume TOK_TEMPLATE parsing (s->token.line_num and
         * s->token.ptr are OK) */
        s->got_lf = FALSE;
        s->last_line_num = s->token.line_num;
        if (js_parse_template_part(s, s->buf_ptr))
          goto done;
        goto handle_template;
      } else if (c != '{') {
        goto done;
      }
      break;
    case TOK_TEMPLATE:
    handle_template:
      if (s->token.u.str.sep != '`') {
        /* '${' inside the template : closing '}' and continue
           parsing the template */
        if (level >= sizeof(state))
          goto done;
        state[level++] = '`';
      }
      break;
    case TOK_EOF:
      goto done;
    case ';':
      if (level == 2) {
        bits |= SKIP_HAS_SEMI;
      }
      break;
    case TOK_ELLIPSIS:
      if (level == 2) {
        bits |= SKIP_HAS_ELLIPSIS;
      }
      break;
    case '=':
      bits |= SKIP_HAS_ASSIGNMENT;
      break;

    case TOK_DIV_ASSIGN:
      tok_len = 2;
      goto parse_regexp;
    case '/':
      tok_len = 1;
    parse_regexp:
      if (is_regexp_allowed(last_tok)) {
        s->buf_ptr -= tok_len;
        if (js_parse_regexp(s)) {
          /* XXX: should clear the exception */
          goto done;
        }
      }
      break;
    }
    /* last_tok is only used to recognize regexps */
    if (s->token.val == TOK_IDENT &&
        (token_is_pseudo_keyword(s, JS_ATOM_of) ||
         token_is_pseudo_keyword(s, JS_ATOM_yield))) {
      last_tok = TOK_OF;
    } else {
      last_tok = s->token.val;
    }
    if (next_token(s)) {
      /* XXX: should clear the exception generated by next_token() */
      break;
    }
    if (level <= 1) {
      tok = s->token.val;
      if (token_is_pseudo_keyword(s, JS_ATOM_of))
        tok = TOK_OF;
      if (no_line_terminator && s->last_line_num != s->token.line_num)
        tok = '\n';
      break;
    }
  }
done:
  if (pbits) {
    *pbits = bits;
  }
  if (js_parse_seek_token(s, &pos))
    return -1;
  return tok;
}

void js_emit_spread_code(JSParseState *s, int depth) {
  int label_rest_next, label_rest_done;

  /* XXX: could check if enum object is an actual array and optimize
     slice extraction. enumeration record and target array are in a
     different order from OP_append case. */
  /* enum_rec xxx -- enum_rec xxx array 0 */
  emit_op(s, OP_array_from);
  emit_u16(s, 0);
  emit_op(s, OP_push_i32);
  emit_u32(s, 0);
  emit_label(s, label_rest_next = new_label(s));
  emit_op(s, OP_for_of_next);
  emit_u8(s, 2 + depth);
  label_rest_done = emit_goto(s, OP_if_true, -1);
  /* array idx val -- array idx */
  emit_op(s, OP_define_array_el);
  emit_op(s, OP_inc);
  emit_goto(s, OP_goto, label_rest_next);
  emit_label(s, label_rest_done);
  /* enum_rec xxx array idx undef -- enum_rec xxx array */
  emit_op(s, OP_drop);
  emit_op(s, OP_drop);
}

static int js_parse_check_duplicate_parameter(JSParseState *s, JSAtom name) {
  /* Check for duplicate parameter names */
  JSFunctionDef *fd = s->cur_func;
  int i;
  for (i = 0; i < fd->arg_count; i++) {
    if (fd->args[i].var_name == name)
      goto duplicate;
  }
  for (i = 0; i < fd->var_count; i++) {
    if (fd->vars[i].var_name == name)
      goto duplicate;
  }
  return 0;

duplicate:
  return js_parse_error(
      s, "duplicate parameter names not allowed in this context");
}

static JSAtom js_parse_destructuring_var(JSParseState *s, int tok, int is_arg) {
  JSAtom name;

  if (!(s->token.val == TOK_IDENT && !s->token.u.ident.is_reserved) ||
      ((s->cur_func->js_mode & JS_MODE_STRICT) &&
       (s->token.u.ident.atom == JS_ATOM_eval ||
        s->token.u.ident.atom == JS_ATOM_arguments))) {
    js_parse_error(s, "invalid destructuring target");
    return JS_ATOM_NULL;
  }
  name = JS_DupAtom(s->ctx, s->token.u.ident.atom);
  if (is_arg && js_parse_check_duplicate_parameter(s, name))
    goto fail;
  if (next_token(s))
    goto fail;

  return name;
fail:
  JS_FreeAtom(s->ctx, name);
  return JS_ATOM_NULL;
}

/* Return -1 if error, 0 if no initializer, 1 if an initializer is
   present at the top level. */
int js_parse_destructuring_element(JSParseState *s, int tok, int is_arg,
                                   int hasval, int has_ellipsis,
                                   BOOL allow_initializer) {
  int label_parse, label_assign, label_done, label_lvalue, depth_lvalue;
  int start_addr, assign_addr;
  JSAtom prop_name, var_name;
  int opcode, scope, tok1, skip_bits;
  BOOL has_initializer;

  if (has_ellipsis < 0) {
    /* pre-parse destruction target for spread detection */
    js_parse_skip_parens_token(s, &skip_bits, FALSE);
    has_ellipsis = skip_bits & SKIP_HAS_ELLIPSIS;
  }

  label_parse = new_label(s);
  label_assign = new_label(s);

  start_addr = s->cur_func->byte_code.size;
  if (hasval) {
    /* consume value from the stack */
    emit_op(s, OP_dup);
    emit_op(s, OP_undefined);
    emit_op(s, OP_strict_eq);
    emit_goto(s, OP_if_true, label_parse);
    emit_label(s, label_assign);
  } else {
    emit_goto(s, OP_goto, label_parse);
    emit_label(s, label_assign);
    /* leave value on the stack */
    emit_op(s, OP_dup);
  }
  assign_addr = s->cur_func->byte_code.size;
  if (s->token.val == '{') {
    if (next_token(s))
      return -1;
    /* throw an exception if the value cannot be converted to an object */
    emit_op(s, OP_to_object);
    if (has_ellipsis) {
      /* add excludeList on stack just below src object */
      emit_op(s, OP_object);
      emit_op(s, OP_swap);
    }
    while (s->token.val != '}') {
      int prop_type;
      if (s->token.val == TOK_ELLIPSIS) {
        if (!has_ellipsis) {
          JS_ThrowInternalError(s->ctx, "unexpected ellipsis token");
          return -1;
        }
        if (next_token(s))
          return -1;
        if (tok) {
          var_name = js_parse_destructuring_var(s, tok, is_arg);
          if (var_name == JS_ATOM_NULL)
            return -1;
          opcode = OP_scope_get_var;
          scope = s->cur_func->scope_level;
          label_lvalue = -1;
          depth_lvalue = 0;
        } else {
          if (js_parse_left_hand_side_expr(s))
            return -1;

          if (get_lvalue(s, &opcode, &scope, &var_name, &label_lvalue,
                         &depth_lvalue, FALSE, '{'))
            return -1;
        }
        if (s->token.val != '}') {
          js_parse_error(s, "assignment rest property must be last");
          goto var_error;
        }
        emit_op(s, OP_object); /* target */
        emit_op(s, OP_copy_data_properties);
        emit_u8(s, 0 | ((depth_lvalue + 1) << 2) | ((depth_lvalue + 2) << 5));
        goto set_val;
      }
      prop_type = js_parse_property_name(s, &prop_name, FALSE, TRUE, FALSE);
      if (prop_type < 0)
        return -1;
      var_name = JS_ATOM_NULL;
      opcode = OP_scope_get_var;
      scope = s->cur_func->scope_level;
      label_lvalue = -1;
      depth_lvalue = 0;
      if (prop_type == PROP_TYPE_IDENT) {
        if (next_token(s))
          goto prop_error;
        if ((s->token.val == '[' ||
             s->token.val == '{') // 紧跟 `:` 之后的是 `[` 或者 `{`
            &&
            ((tok1 = js_parse_skip_parens_token(s, &skip_bits, FALSE)) == ',' ||
             tok1 == '=' || tok1 == '}')) {
          if (prop_name == JS_ATOM_NULL) {
            /* computed property name on stack */
            if (has_ellipsis) {
              /* define the property in excludeList */
              emit_op(s, OP_to_propkey); /* avoid calling ToString twice */
              emit_op(s, OP_perm3);      /* TOS: src excludeList prop */
              emit_op(s, OP_null);       /* TOS: src excludeList prop null */
              emit_op(s, OP_define_array_el); /* TOS: src excludeList prop */
              emit_op(s, OP_perm3);           /* TOS: excludeList src prop */
            }
            /* get the computed property from the source object */
            emit_op(s, OP_get_array_el2);
          } else {
            /* named property */
            if (has_ellipsis) {
              /* define the property in excludeList */
              emit_op(s, OP_swap);         /* TOS: src excludeList */
              emit_op(s, OP_null);         /* TOS: src excludeList null */
              emit_op(s, OP_define_field); /* TOS: src excludeList */
              emit_atom(s, prop_name);
              emit_op(s, OP_swap); /* TOS: excludeList src */
            }
            /* get the named property from the source object */
            emit_op(s, OP_get_field2);
            emit_u32(s, prop_name);
          }
          if (js_parse_destructuring_element(s, tok, is_arg, TRUE, -1, TRUE) <
              0)
            return -1;
          if (s->token.val == '}')
            break;
          /* accept a trailing comma before the '}' */
          if (js_parse_expect(s, ','))
            return -1;
          continue;
        }
        if (prop_name == JS_ATOM_NULL) {
          emit_op(s, OP_to_propkey2);
          if (has_ellipsis) {
            /* define the property in excludeList */
            emit_op(s, OP_perm3);
            emit_op(s, OP_null);
            emit_op(s, OP_define_array_el);
            emit_op(s, OP_perm3);
          }
          /* source prop -- source source prop */
          emit_op(s, OP_dup1);
        } else {
          if (has_ellipsis) {
            /* define the property in excludeList */
            emit_op(s, OP_swap);
            emit_op(s, OP_null);
            emit_op(s, OP_define_field);
            emit_atom(s, prop_name);
            emit_op(s, OP_swap);
          }
          /* source -- source source */
          emit_op(s, OP_dup);
        }
        if (tok) {
          var_name = js_parse_destructuring_var(s, tok, is_arg);
          if (var_name == JS_ATOM_NULL)
            goto prop_error;
        } else {
          if (js_parse_left_hand_side_expr(s))
            goto prop_error;
        lvalue:
          if (get_lvalue(s, &opcode, &scope, &var_name, &label_lvalue,
                         &depth_lvalue, FALSE, '{'))
            goto prop_error;
          /* swap ref and lvalue object if any */
          if (prop_name == JS_ATOM_NULL) {
            switch (depth_lvalue) {
            case 1:
              /* source prop x -> x source prop */
              emit_op(s, OP_rot3r);
              break;
            case 2:
              /* source prop x y -> x y source prop */
              emit_op(s, OP_swap2); /* t p2 s p1 */
              break;
            case 3:
              /* source prop x y z -> x y z source prop */
              emit_op(s, OP_rot5l);
              emit_op(s, OP_rot5l);
              break;
            }
          } else {
            switch (depth_lvalue) {
            case 1:
              /* source x -> x source */
              emit_op(s, OP_swap);
              break;
            case 2:
              /* source x y -> x y source */
              emit_op(s, OP_rot3l);
              break;
            case 3:
              /* source x y z -> x y z source */
              emit_op(s, OP_rot4l);
              break;
            }
          }
        }
        if (prop_name == JS_ATOM_NULL) {
          /* computed property name on stack */
          /* XXX: should have OP_get_array_el2x with depth */
          /* source prop -- val */
          emit_op(s, OP_get_array_el);
        } else {
          /* named property */
          /* XXX: should have OP_get_field2x with depth */
          /* source -- val */
          emit_op(s, OP_get_field);
          emit_u32(s, prop_name);
        }
      } else {
        /* prop_type = PROP_TYPE_VAR, cannot be a computed property */
        if (is_arg && js_parse_check_duplicate_parameter(s, prop_name))
          goto prop_error;
        if ((s->cur_func->js_mode & JS_MODE_STRICT) &&
            (prop_name == JS_ATOM_eval || prop_name == JS_ATOM_arguments)) {
          js_parse_error(s, "invalid destructuring target");
          goto prop_error;
        }
        if (has_ellipsis) {
          /* define the property in excludeList */
          emit_op(s, OP_swap);
          emit_op(s, OP_null);
          emit_op(s, OP_define_field);
          emit_atom(s, prop_name);
          emit_op(s, OP_swap);
        }
        if (!tok || tok == TOK_VAR) {
          /* generate reference */
          /* source -- source source */
          emit_op(s, OP_dup);
          emit_op(s, OP_scope_get_var);
          emit_atom(s, prop_name);
          emit_u16(s, s->cur_func->scope_level);
          goto lvalue;
        }
        var_name = JS_DupAtom(s->ctx, prop_name);
        /* source -- source val */
        emit_op(s, OP_get_field2);
        emit_u32(s, prop_name);
      }
    set_val:
      if (tok) {
        if (js_define_var(s, var_name, tok))
          goto var_error;
        scope = s->cur_func->scope_level;
      }
      if (s->token.val == '=') { /* handle optional default value */
        int label_hasval;
        emit_op(s, OP_dup);
        emit_op(s, OP_undefined);
        emit_op(s, OP_strict_eq);
        label_hasval = emit_goto(s, OP_if_false, -1);
        if (next_token(s))
          goto var_error;
        emit_op(s, OP_drop);
        if (js_parse_assign_expr(s))
          goto var_error;
        if (opcode == OP_scope_get_var || opcode == OP_get_ref_value)
          set_object_name(s, var_name);
        emit_label(s, label_hasval);
      }
      /* store value into lvalue object */
      put_lvalue(s, opcode, scope, var_name, label_lvalue,
                 PUT_LVALUE_NOKEEP_DEPTH, (tok == TOK_CONST || tok == TOK_LET));
      if (s->token.val == '}')
        break;
      /* accept a trailing comma before the '}' */
      if (js_parse_expect(s, ','))
        return -1;
    }
    /* drop the source object */
    emit_op(s, OP_drop);
    if (has_ellipsis) {
      emit_op(s, OP_drop); /* pop excludeList */
    }
    if (next_token(s))
      return -1;
  } else if (s->token.val == '[') {
    BOOL has_spread;
    int enum_depth;
    BlockEnv block_env;

    if (next_token(s))
      return -1;
    /* the block environment is only needed in generators in case
       'yield' triggers a 'return' */
    push_break_entry(s->cur_func, &block_env, JS_ATOM_NULL, -1, -1, 2);
    block_env.has_iterator = TRUE;
    emit_op(s, OP_for_of_start);
    has_spread = FALSE;
    while (s->token.val != ']') {
      /* get the next value */
      if (s->token.val == TOK_ELLIPSIS) {
        if (next_token(s))
          return -1;
        if (s->token.val == ',' || s->token.val == ']')
          return js_parse_error(s, "missing binding pattern...");
        has_spread = TRUE;
      }
      if (s->token.val == ',') {
        /* do nothing, skip the value, has_spread is false */
        emit_op(s, OP_for_of_next);
        emit_u8(s, 0);
        emit_op(s, OP_drop);
        emit_op(s, OP_drop);
      } else if ((s->token.val == '[' || s->token.val == '{') &&
                 ((tok1 = js_parse_skip_parens_token(s, &skip_bits, FALSE)) ==
                      ',' ||
                  tok1 == '=' || tok1 == ']')) {
        if (has_spread) {
          if (tok1 == '=')
            return js_parse_error(s,
                                  "rest element cannot have a default value");
          js_emit_spread_code(s, 0);
        } else {
          emit_op(s, OP_for_of_next);
          emit_u8(s, 0);
          emit_op(s, OP_drop);
        }
        if (js_parse_destructuring_element(
                s, tok, is_arg, TRUE, skip_bits & SKIP_HAS_ELLIPSIS, TRUE) < 0)
          return -1;
      } else {
        var_name = JS_ATOM_NULL;
        enum_depth = 0;
        if (tok) {
          var_name = js_parse_destructuring_var(s, tok, is_arg);
          if (var_name == JS_ATOM_NULL)
            goto var_error;
          if (js_define_var(s, var_name, tok))
            goto var_error;
          opcode = OP_scope_get_var;
          scope = s->cur_func->scope_level;
        } else {
          if (js_parse_left_hand_side_expr(s))
            return -1;
          if (get_lvalue(s, &opcode, &scope, &var_name, &label_lvalue,
                         &enum_depth, FALSE, '[')) {
            return -1;
          }
        }
        if (has_spread) {
          js_emit_spread_code(s, enum_depth);
        } else {
          emit_op(s, OP_for_of_next);
          emit_u8(s, enum_depth);
          emit_op(s, OP_drop);
        }
        if (s->token.val == '=' && !has_spread) {
          /* handle optional default value */
          int label_hasval;
          emit_op(s, OP_dup);
          emit_op(s, OP_undefined);
          emit_op(s, OP_strict_eq);
          label_hasval = emit_goto(s, OP_if_false, -1);
          if (next_token(s))
            goto var_error;
          emit_op(s, OP_drop);
          if (js_parse_assign_expr(s))
            goto var_error;
          if (opcode == OP_scope_get_var || opcode == OP_get_ref_value)
            set_object_name(s, var_name);
          emit_label(s, label_hasval);
        }
        /* store value into lvalue object */
        put_lvalue(s, opcode, scope, var_name, label_lvalue,
                   PUT_LVALUE_NOKEEP_DEPTH,
                   (tok == TOK_CONST || tok == TOK_LET));
      }
      if (s->token.val == ']')
        break;
      if (has_spread)
        return js_parse_error(s, "rest element must be the last one");
      /* accept a trailing comma before the ']' */
      if (js_parse_expect(s, ','))
        return -1;
    }
    /* close iterator object:
       if completed, enum_obj has been replaced by undefined */
    emit_op(s, OP_iterator_close);
    pop_break_entry(s->cur_func);
    if (next_token(s))
      return -1;
  } else {
    return js_parse_error(s, "invalid assignment syntax");
  }
  if (s->token.val == '=' && allow_initializer) {
    label_done = emit_goto(s, OP_goto, -1);
    if (next_token(s))
      return -1;
    emit_label(s, label_parse);
    if (hasval)
      emit_op(s, OP_drop);
    if (js_parse_assign_expr(s))
      return -1;
    emit_goto(s, OP_goto, label_assign);
    emit_label(s, label_done);
    has_initializer = TRUE;
  } else {
    /* normally hasval is true except if
       js_parse_skip_parens_token() was wrong in the parsing */
    //        assert(hasval);
    if (!hasval) {
      js_parse_error(s, "too complicated destructuring expression");
      return -1;
    }
    /* remove test and decrement label ref count */
    memset(s->cur_func->byte_code.buf + start_addr, OP_nop,
           assign_addr - start_addr);
    s->cur_func->label_slots[label_parse].ref_count--;
    has_initializer = FALSE;
  }
  return has_initializer;

prop_error:
  JS_FreeAtom(s->ctx, prop_name);
var_error:
  JS_FreeAtom(s->ctx, var_name);
  return -1;
}

__exception int js_define_var(JSParseState *s, JSAtom name, int tok) {
  JSFunctionDef *fd = s->cur_func;
  JSVarDefEnum var_def_type;

  if (name == JS_ATOM_yield && fd->func_kind == JS_FUNC_GENERATOR) {
    return js_parse_error(s, "yield is a reserved identifier");
  }
  if ((name == JS_ATOM_arguments || name == JS_ATOM_eval) &&
      (fd->js_mode & JS_MODE_STRICT)) {
    return js_parse_error(s, "invalid variable name in strict mode");
  }
  if ((name == JS_ATOM_let || name == JS_ATOM_undefined) &&
      (tok == TOK_LET || tok == TOK_CONST)) {
    return js_parse_error(s, "invalid lexical variable name");
  }
  switch (tok) {
  case TOK_LET:
    var_def_type = JS_VAR_DEF_LET;
    break;
  case TOK_CONST:
    var_def_type = JS_VAR_DEF_CONST;
    break;
  case TOK_VAR:
    var_def_type = JS_VAR_DEF_VAR;
    break;
  case TOK_CATCH:
    var_def_type = JS_VAR_DEF_CATCH;
    break;
  default:
    abort();
  }
  if (define_var(s, fd, name, var_def_type) < 0)
    return -1;
  return 0;
}

/* allowed parse_flags: PF_IN_ACCEPTED */
__exception int js_parse_var(JSParseState *s, int parse_flags, int tok,
                             BOOL export_flag) {
  JSContext *ctx = s->ctx;
  JSFunctionDef *fd = s->cur_func;
  JSAtom name = JS_ATOM_NULL;

  for (;;) {
    if (s->token.val == TOK_IDENT) {
      if (s->token.u.ident.is_reserved) {
        return js_parse_error_reserved_identifier(s);
      }
      name = JS_DupAtom(ctx, s->token.u.ident.atom);
      if (name == JS_ATOM_let && (tok == TOK_LET || tok == TOK_CONST)) {
        js_parse_error(s, "'let' is not a valid lexical identifier");
        goto var_error;
      }
      if (next_token(s))
        goto var_error;
      if (js_define_var(s, name, tok))
        goto var_error;
      if (export_flag) {
        if (!add_export_entry(s, s->cur_func->module, name, name,
                              JS_EXPORT_TYPE_LOCAL))
          goto var_error;
      }

      if (s->token.val == '=') {
        if (next_token(s))
          goto var_error;
        if (tok == TOK_VAR) {
          /* Must make a reference for proper `with` semantics */
          int opcode, scope, label;
          JSAtom name1;

          emit_op(s, OP_scope_get_var);
          emit_atom(s, name);
          emit_u16(s, fd->scope_level);
          if (get_lvalue(s, &opcode, &scope, &name1, &label, NULL, FALSE, '=') <
              0)
            goto var_error;
          if (js_parse_assign_expr2(s, parse_flags)) {
            JS_FreeAtom(ctx, name1);
            goto var_error;
          }
          set_object_name(s, name);
          put_lvalue(s, opcode, scope, name1, label, PUT_LVALUE_NOKEEP, FALSE);
        } else {
          if (js_parse_assign_expr2(s, parse_flags))
            goto var_error;
          set_object_name(s, name);
          emit_op(s, (tok == TOK_CONST || tok == TOK_LET)
                         ? OP_scope_put_var_init
                         : OP_scope_put_var);
          emit_atom(s, name);
          emit_u16(s, fd->scope_level);
        }
      } else {
        if (tok == TOK_CONST) {
          js_parse_error(s, "missing initializer for const variable");
          goto var_error;
        }
        if (tok == TOK_LET) {
          /* initialize lexical variable upon entering its scope */
          emit_op(s, OP_undefined);
          emit_op(s, OP_scope_put_var_init);
          emit_atom(s, name);
          emit_u16(s, fd->scope_level);
        }
      }
      JS_FreeAtom(ctx, name);
    } else {
      int skip_bits;
      if ((s->token.val == '[' || s->token.val == '{') &&
          js_parse_skip_parens_token(s, &skip_bits, FALSE) == '=') {
        emit_op(s, OP_undefined);
        if (js_parse_destructuring_element(
                s, tok, 0, TRUE, skip_bits & SKIP_HAS_ELLIPSIS, TRUE) < 0)
          return -1;
      } else {
        return js_parse_error(s, "variable name expected");
      }
    }
    if (s->token.val != ',')
      break;
    if (next_token(s))
      return -1;
  }
  return 0;

var_error:
  JS_FreeAtom(ctx, name);
  return -1;
}