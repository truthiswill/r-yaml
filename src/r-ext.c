#include <stdio.h>
#include <stdlib.h>
#include "R.h"
#include "Rdefines.h"
#include "R_ext/Rdynload.h"
#include "yaml.h"
#include "yaml_private.h"

static SEXP R_KeysSymbol = NULL;

yaml_char_t *find_implicit_tag(yaml_char_t *value, size_t size);

typedef struct {
  int refcount;
  SEXP obj;

  /* This is for tracking whether or not this object has a parent.
   * If there is no parent, that means this object should be UNPROTECT'd
   * when assigned to a parent SEXP object */
  int orphan;
} s_prot_object;

typedef struct {
  s_prot_object *obj;
  int placeholder;
  void *prev;
} s_stack_entry;

typedef struct {
  yaml_char_t *name;
  s_prot_object *obj;
  void *prev;
} s_alias_entry;

static s_prot_object *
new_prot_object(obj)
  SEXP obj;
{
  s_prot_object *result;

  result = (s_prot_object *)malloc(sizeof(s_prot_object));
  result->refcount = 0;
  result->obj = obj;
  result->orphan = 1;

  return result;
}

static void
prune_prot_object(obj)
  s_prot_object *obj;
{
  if (obj->orphan == 1) {
    /* obj is now part of list and is therefore protected */
    UNPROTECT_PTR(obj->obj);
    obj->orphan = 0;
  }

  if (obj->refcount == 0) {
    /* Don't need this object wrapper anymore */
    free(obj);
  }
}

static void
stack_push(stack, placeholder, obj)
  s_stack_entry **stack;
  int placeholder;
  s_prot_object *obj;
{
  s_stack_entry *result;

  result = (s_stack_entry *)malloc(sizeof(s_stack_entry));
  result->placeholder = placeholder;
  result->obj = obj;
  obj->refcount++;
  result->prev = *stack;

  *stack = result;
}

static void
stack_pop(stack, obj)
  s_stack_entry **stack;
  s_prot_object **obj;
{
  s_stack_entry *result, *top;

  top = *stack;
  *obj = top->obj;
  top->obj->refcount--;
  result = (s_stack_entry *)top->prev;
  free(top);

  *stack = result;

}

static void
handle_alias(event, stack, aliases)
  yaml_event_t *event;
  s_stack_entry **stack;
  s_alias_entry *aliases;
{
  s_alias_entry *alias = aliases;
  while (alias) {
    if (strcmp((char *)alias->name, event->data.alias.anchor) == 0) {
      stack_push(stack, 0, alias->obj);
      break;
    }
  }
}

static void
handle_start_event(event, stack)
  yaml_event_t *event;
  s_stack_entry **stack;
{
  stack_push(stack, 1, new_prot_object(NULL, -1));
}

static void
handle_scalar(event, stack, s_handlers)
  yaml_event_t *event;
  s_stack_entry **stack;
  SEXP s_handlers;
{
  SEXP obj, names, cmd, tmp_obj;
  yaml_char_t *tag, *value;
  size_t len;
  int handled = 0, errorOccurred = 0, i;
  PROTECT_INDEX ipx;

  tag = event->data.scalar.tag;
  value = event->data.scalar.value;
  if (tag == NULL || strcmp(tag, "!") == 0) {
    /* If there's no tag, try to tag it */
    tag = find_implicit_tag(value, len);
  }

  PROTECT_WITH_INDEX(obj = NEW_STRING(1), &ipx);
  SET_STRING_ELT(obj, 0, mkChar(value));

  /* Look for a custom R handler */
  if (s_handlers != R_NilValue) {
    names = GET_NAMES(s_handlers);
    for (i = 0; i < length(names); i++) {
      if (STRING_ELT(names, i) != NA_STRING) {
        if (strcmp(translateChar(STRING_ELT(names, i)), tag) == 0) {
          /* Found custom handler */
          handled = 1;

          PROTECT(cmd = allocVector(LANGSXP, 2));
          SETCAR(cmd, VECTOR_ELT(s_handlers, i));
          SETCADR(cmd, obj);
          tmp_obj = R_tryEval(cmd, R_GlobalEnv, &errorOccurred);
          UNPROTECT(1);

          if (errorOccurred) {
            warning("an error occurred when handling type '%s'; returning default object", tag);
          }
          else {
            obj = tmp_obj;
            REPROTECT(obj, ipx);
          }
          break;
        }
      }
    }
  }

  stack_push(stack, 0, new_prot_object(obj));
}

static void
handle_sequence(event, stack)
  yaml_event_t *event;
  s_stack_entry **stack;
{
  s_stack_entry *stack_ptr;
  s_prot_object *obj;
  int count, i;
  SEXP list;

  /* Find out how many elements there are */
  stack_ptr = *stack;
  count = 0;
  while (!stack_ptr->placeholder) {
    count++;
    stack_ptr = stack_ptr->prev;
  }

  /* Initialize list */
  list = allocVector(VECSXP, count);
  PROTECT(list);

  /* Populate the list, popping items off the stack as we go */
  for (i = count - 1; i >= 0; i--) {
    stack_pop(stack, &obj);
    SET_VECTOR_ELT(list, i, obj->obj);
    prune_prot_object(obj);
  }
  (*stack)->obj->obj = list;
  (*stack)->placeholder = 0;
#if DEBUG
  PrintValue(list);
#endif
}

static void
handle_map(event, stack, coerce)
  yaml_event_t *event;
  s_stack_entry **stack;
  int coerce;
{
  s_prot_object *obj;
  s_stack_entry *stack_ptr;
  int count, i, orphan_key;
  SEXP list, keys, key, key_str;
  PROTECT_INDEX ipx;

  /* Find out how many elements there are */
  stack_ptr = *stack;
  count = 0;
  while (!stack_ptr->placeholder) {
    count++;
    stack_ptr = stack_ptr->prev;
  }

  /* Initialize value list */
  list = allocVector(VECSXP, count / 2);
  PROTECT_WITH_INDEX(list, &ipx);

  /* Initialize key list/vector */
  if (coerce) {
    PROTECT(keys = NEW_STRING(count / 2));
  }
  else {
    PROTECT(keys = allocVector(VECSXP, count / 2));
  }

  /* Populate the list, popping items off the stack as we go */
  for (i = count - 1; i >= 0; i--) {
    stack_pop(stack, &obj);

    if (i % 2 == 1) {
      /* map value */
      SET_VECTOR_ELT(list, i / 2, obj->obj);
    }
    else {
      /* map key */
      /* TODO: handle duplicate keys */
      if (coerce) {
        key = AS_CHARACTER(obj->obj);
        orphan_key = (key != obj->obj);
        if (orphan_key) {
          /* This key has been coerced into a character, and is a new object. */
          PROTECT(key);
        }

        switch (LENGTH(key)) {
          case 0:
            warning("Empty character vector used as a list name");
            key_str = mkChar("");
            break;
          default:
            warning("Character vector of length greater than 1 used as a list name");
          case 1:
            key_str = STRING_ELT(key, 0);
            break;
        }
        SET_STRING_ELT(keys, i / 2, key_str);

        if (orphan_key) {
          UNPROTECT(1);
        }
      }
      else {
        SET_VECTOR_ELT(keys, i / 2, obj->obj);
      }
    }
    prune_prot_object(obj);
  }

  if (coerce) {
    SET_NAMES(list, keys);
  }
  else {
    setAttrib(list, R_KeysSymbol, keys);
  }
  UNPROTECT_PTR(keys);

  (*stack)->obj->obj = list;
  (*stack)->placeholder = 0;
#if DEBUG
  PrintValue(list);
#endif
}

static void
possibly_record_alias(anchor, aliases, obj)
  yaml_char_t *anchor;
  s_alias_entry **aliases;
  s_prot_object *obj;
{
  s_alias_entry *alias;

  if (anchor != NULL) {
    alias = (s_alias_entry *)malloc(sizeof(s_alias_entry));
    alias->name = yaml_strdup(anchor);
    alias->obj = obj;
    obj->refcount++;
    alias->prev = *aliases;
    *aliases = alias;
  }
}

static int
R_is_named_list(obj)
  SEXP obj;
{
  SEXP names;
  if (TYPEOF(obj) != VECSXP)
    return 0;

  names = GET_NAMES(obj);
  return (TYPEOF(names) == STRSXP && LENGTH(names) == LENGTH(obj));
}

SEXP
load_yaml_str(s_str, s_use_named, s_handlers)
  SEXP s_str;
  SEXP s_use_named;
  SEXP s_handlers;
{
  s_prot_object *obj;
  SEXP retval, R_hndlr, cmd, names;
  yaml_parser_t parser;
  yaml_event_t event;
  const char *str, *name;
  char error_msg[255];
  long len;
  int use_named, i, done = 0;
  s_stack_entry *stack = NULL;
  s_alias_entry *aliases = NULL, *alias;

  if (!isString(s_str) || length(s_str) != 1) {
    error("first argument must be a character vector of length 1");
    return R_NilValue;
  }

  if (!isLogical(s_use_named) || length(s_use_named) != 1) {
    error("second argument must be a logical vector of length 1");
    return R_NilValue;
  }

  if (s_handlers == R_NilValue) {
    // Do nothing
  }
  else if (!R_is_named_list(s_handlers)) {
    error("handlers must be either NULL or a named list of functions");
    return R_NilValue;
  }
  else {
    names = GET_NAMES(s_handlers);
    for (i = 0; i < LENGTH(names); i++) {
      name = CHAR(STRING_ELT(names, i));
      R_hndlr = VECTOR_ELT(s_handlers, i);

      if (TYPEOF(R_hndlr) != CLOSXP) {
        warning("your handler for '%s' is not a function; using default", name);
        continue;
      }

      /* custom handlers for merge, default, and anchor#bad are illegal */
      if ( strcmp( name, "merge" ) == 0   ||
           strcmp( name, "default" ) == 0 ||
           strcmp( name, "anchor#bad" ) == 0 )
      {
        warning("custom handling of %s type is not allowed; handler ignored", name);
        continue;
      }
    }
  }

  str = CHAR(STRING_ELT(s_str, 0));
  len = LENGTH(STRING_ELT(s_str, 0));
  use_named = LOGICAL(s_use_named)[0];

  yaml_parser_initialize(&parser);
  yaml_parser_set_input_string(&parser, str, len);

  error_msg[0] = 0;
  while (!done) {
    if (yaml_parser_parse(&parser, &event)) {
      switch (event.type) {
        case YAML_ALIAS_EVENT:
#if DEBUG
          printf("ALIAS: %s\n", event.data.alias.anchor);
#endif
          handle_alias(&event, &stack, aliases);
          break;

        case YAML_SCALAR_EVENT:
#if DEBUG
          printf("SCALAR: %s (%s)\n", event.data.scalar.value, event.data.scalar.tag);
#endif
          handle_scalar(&event, &stack, s_handlers);
          possibly_record_alias(event.data.scalar.anchor, &aliases, stack->obj);
          break;

        case YAML_SEQUENCE_START_EVENT:
#if DEBUG
          printf("SEQUENCE START: (%s)\n", event.data.sequence_start.tag);
#endif
          handle_start_event(&event, &stack);
          possibly_record_alias(event.data.sequence_start.anchor, &aliases, stack->obj);
          break;

        case YAML_SEQUENCE_END_EVENT:
#if DEBUG
          printf("SEQUENCE END\n");
#endif
          handle_sequence(&event, &stack);
          break;

        case YAML_MAPPING_START_EVENT:
#if DEBUG
          printf("MAPPING START: (%s)\n", event.data.mapping_start.tag);
#endif
          handle_start_event(&event, &stack);
          possibly_record_alias(event.data.mapping_start.anchor, &aliases, stack->obj);
          break;

        case YAML_MAPPING_END_EVENT:
#if DEBUG
          printf("MAPPING END\n");
#endif
          handle_map(&event, &stack, use_named);
          break;

        case YAML_STREAM_END_EVENT:
          if (stack != NULL) {
            stack_pop(&stack, &obj);
            retval = obj->obj;
            prune_prot_object(obj);
          }
          else {
            retval = R_NilValue;
          }

          done = 1;
          break;
      }
    }
    else {
      retval = R_NilValue;

      /* Parser error */
      switch (parser.error) {
        case YAML_MEMORY_ERROR:
          sprintf(error_msg, "Memory error: Not enough memory for parsing");
          break;

        case YAML_READER_ERROR:
          if (parser.problem_value != -1) {
            sprintf(error_msg, "Reader error: %s: #%X at %zd", parser.problem,
              parser.problem_value, parser.problem_offset);
          }
          else {
            sprintf(error_msg, "Reader error: %s at %zd", parser.problem,
              parser.problem_offset);
          }
          break;

        case YAML_SCANNER_ERROR:
          if (parser.context) {
            sprintf(error_msg, "Scanner error: %s at line %zd, column %zd"
              "%s at line %zd, column %zd", parser.context,
              parser.context_mark.line+1, parser.context_mark.column+1,
              parser.problem, parser.problem_mark.line+1,
              parser.problem_mark.column+1);
          }
          else {
            sprintf(error_msg, "Scanner error: %s at line %zd, column %zd",
              parser.problem, parser.problem_mark.line+1,
              parser.problem_mark.column+1);
          }
          break;

        case YAML_PARSER_ERROR:
          if (parser.context) {
            sprintf(error_msg, "Parser error: %s at line %zd, column %zd"
              "%s at line %zd, column %zd", parser.context,
              parser.context_mark.line+1, parser.context_mark.column+1,
              parser.problem, parser.problem_mark.line+1,
              parser.problem_mark.column+1);
          }
          else {
            sprintf(error_msg, "Parser error: %s at line %zd, column %zd",
              parser.problem, parser.problem_mark.line+1,
              parser.problem_mark.column+1);
          }
          break;

        default:
          /* Couldn't happen. */
          sprintf(error_msg, "Internal error");
          break;
      }
      done = 1;
    }

    yaml_event_delete(&event);
  }

  /* Clean up aliases */
  while (aliases != NULL) {
    alias = aliases;
    aliases = aliases->prev;
    free(alias->name);
    free(alias->obj);
    free(alias);
  }

  yaml_parser_delete(&parser);

  if (error_msg[0] != 0) {
    error(error_msg);
  }

  return retval;
}

R_CallMethodDef callMethods[] = {
  {"yaml.load",(DL_FUNC)&load_yaml_str, 3},
  {NULL,NULL, 0}
};

void R_init_yaml(DllInfo *dll) {
  R_KeysSymbol = install("keys");
  R_registerRoutines(dll,NULL,callMethods,NULL,NULL);
}
