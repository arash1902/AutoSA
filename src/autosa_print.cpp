/* Helper functions in codegen */
#include <assert.h>

#include "autosa_print.h"
#include "autosa_utils.h"
#include "autosa_comm.h"
#include "print.h"

/* Print the call of an array argument.
 */
__isl_give isl_printer *autosa_array_info_print_call_argument(
    __isl_take isl_printer *p, struct autosa_array_info *array, int n_ref)
{
  if (autosa_array_is_read_only_scalar(array))
    return isl_printer_print_str(p, array->name);

  p = isl_printer_print_str(p, "buffer_");
  p = isl_printer_print_str(p, array->name);
  if (n_ref >= 0)
  {
    std::pair<int, int> ref_port_map = array->local_array->group_ref_mem_port_map[n_ref];
    p = isl_printer_print_str(p, "[");
    p = isl_printer_print_int(p, ref_port_map.second);
    p = isl_printer_print_str(p, "]");
  }

  return p;
}

/* Print the array group name prefix.
 * [array_name]_[group_id](optional)_[drain](optional)
 */
__isl_give isl_printer *autosa_array_ref_group_print_prefix(
    struct autosa_array_ref_group *group, __isl_take isl_printer *p)
{
  p = isl_printer_print_str(p, group->array->name);
  if (group->group_type == AUTOSA_DRAIN_GROUP)
  {
    p = isl_printer_print_str(p, "_drain");
  }
  else
  {
    if (group->group_type == AUTOSA_IO_GROUP && group->local_array->n_io_group > 1)
    {
      p = isl_printer_print_str(p, "_");
      p = isl_printer_print_int(p, group->nr);
    }
    else if (group->group_type == AUTOSA_PE_GROUP && group->local_array->n_pe_group > 1)
    {
      p = isl_printer_print_str(p, "_");
      p = isl_printer_print_int(p, group->nr);
    }
  }

  return p;
}

/* Print the name of the local copy of a given group of array references.
 */
__isl_give isl_printer *autosa_array_ref_group_print_fifo_name(
    struct autosa_array_ref_group *group, __isl_take isl_printer *p)
{
  int global = 0;
  enum autosa_group_access_type type;

  if (group->group_type == AUTOSA_PE_GROUP)
    return p;

  p = isl_printer_print_str(p, "fifo_");
  p = isl_printer_print_str(p, group->array->name);
  if (group->local_array->n_io_group > 1)
  {
    p = isl_printer_print_str(p, "_");
    p = isl_printer_print_int(p, group->nr);
  }
  if (group->group_type == AUTOSA_DRAIN_GROUP)
  {
    p = isl_printer_print_str(p, "_drain");
  }

  return p;
}

/* Was the definition of "type" printed before?
 * That is, does its name appear in the list of printed types "types"?
 */
static int already_printed(struct autosa_types *types,
                           struct pet_type *type)
{
  int i;

  for (i = 0; i < types->n; ++i)
    if (!strcmp(types->name[i], type->name))
      return 1;

  return 0;
}

/* Print the definitions of all types prog->scop that have not been
 * printed before (according to "types") on "p".
 * Extend the list of printed types "types" with the newly printed types.
 */
__isl_give isl_printer *autosa_print_types(__isl_take isl_printer *p,
                                           struct autosa_types *types, struct autosa_prog *prog)
{
  int i, n;
  isl_ctx *ctx;
  char **name;

  n = prog->scop->pet->n_type;

  if (n == 0)
    return p;

  ctx = isl_printer_get_ctx(p);
  name = isl_realloc_array(ctx, types->name, char *, types->n + n);
  if (!name)
    return isl_printer_free(p);
  types->name = name;

  for (i = 0; i < n; ++i)
  {
    struct pet_type *type = prog->scop->pet->types[i];

    if (already_printed(types, type))
      continue;

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, type->definition);
    p = isl_printer_print_str(p, ";");
    p = isl_printer_end_line(p);

    types->name[types->n++] = strdup(type->name);
  }

  return p;
}

/* Print declarations to "p" for arrays that are local to "prog"
 * but that are used on the host and therefore require a declaration.
 */
__isl_give isl_printer *autosa_print_local_declarations(
    __isl_take isl_printer *p, struct autosa_prog *prog)
{
  int i;

  if (!prog)
    return isl_printer_free(p);

  for (i = 0; i < prog->n_array; ++i)
  {
    struct autosa_array_info *array = &prog->array[i];
    isl_ast_expr *size;

    if (!array->declare_local)
      continue;
    size = array->declared_size;
    p = ppcg_print_declaration_with_size(p, array->type, size);
  }

  return p;
}

__isl_give isl_printer *print_str_new_line(__isl_take isl_printer *p, const char *str)
{
  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, str);
  p = isl_printer_end_line(p);

  return p;
}

/* Print an expression for the size of "array" in data items.
 */
__isl_give isl_printer *autosa_array_info_print_data_size(
    __isl_take isl_printer *p, struct autosa_array_info *array)
{
  int i;
  int first = 1;

  for (i = 0; i < array->n_index; ++i)
  {
    if (!first)
      p = isl_printer_print_str(p, " * ");

    isl_ast_expr *bound;

    p = isl_printer_print_str(p, "(");
    bound = isl_ast_expr_get_op_arg(array->bound_expr, 1 + i);
    p = isl_printer_print_ast_expr(p, bound);
    isl_ast_expr_free(bound);
    p = isl_printer_print_str(p, ")");
    first = 0;
  }

  return p;
}

/* Print an expression for the size of "array" in bytes.
 */
__isl_give isl_printer *autosa_array_info_print_size(
    __isl_take isl_printer *p, struct autosa_array_info *array)
{
  int i;

  for (i = 0; i < array->n_index; ++i)
  {
    isl_ast_expr *bound;

    p = isl_printer_print_str(p, "(");
    bound = isl_ast_expr_get_op_arg(array->bound_expr, 1 + i);
    p = isl_printer_print_ast_expr(p, bound);
    isl_ast_expr_free(bound);
    p = isl_printer_print_str(p, ") * ");
  }
  p = isl_printer_print_str(p, "sizeof(");
  p = isl_printer_print_str(p, array->type);
  p = isl_printer_print_str(p, ")");

  return p;
}

__isl_give isl_printer *autosa_print_array_type(__isl_take isl_printer *p,
                                                struct autosa_array_info *array)
{
  int n_lane = array->n_lane;
  if (n_lane == 1)
    p = isl_printer_print_str(p, array->type);
  else
  {
    p = isl_printer_print_str(p, array->name);
    p = isl_printer_print_str(p, "_t");
    p = isl_printer_print_int(p, n_lane);
  }

  return p;
}

__isl_give isl_printer *autosa_kernel_print_domain(__isl_take isl_printer *p,
                                                   struct autosa_kernel_stmt *stmt)
{
  return pet_stmt_print_body(stmt->u.d.stmt->stmt, p, stmt->u.d.ref2expr);
}

/* Print the declaration of a non-linearized array argument.
 */
static __isl_give isl_printer *print_non_linearized_declaration_argument(
    __isl_take isl_printer *p, struct autosa_array_info *array, int n_lane)
{
  if (n_lane == 1)
  {
    p = isl_printer_print_str(p, array->type);
    p = isl_printer_print_str(p, " ");

    p = isl_printer_print_ast_expr(p, array->bound_expr);
  }
  else
  {
    p = isl_printer_print_str(p, array->name);
    p = isl_printer_print_str(p, "_t");
    p = isl_printer_print_int(p, n_lane);

    p = isl_printer_print_ast_expr(p, array->bound_expr); // TODO
  }

  return p;
}

/* Print the declaration of an array argument.
 * "memory_space" allows to specify a memory space prefix.
 */
__isl_give isl_printer *autosa_array_info_print_declaration_argument(
    __isl_take isl_printer *p, struct autosa_array_info *array, int n_lane,
    const char *memory_space, int n_ref)
{
  if (autosa_array_is_read_only_scalar(array))
  {
    p = isl_printer_print_str(p, array->type);
    p = isl_printer_print_str(p, " ");
    p = isl_printer_print_str(p, array->name);
    return p;
  }

  if (memory_space)
  {
    p = isl_printer_print_str(p, memory_space);
    p = isl_printer_print_str(p, " ");
  }

  if (array->n_index != 0 && !array->linearize)
    return print_non_linearized_declaration_argument(p, array, n_lane);

  if (n_lane == 1)
    p = isl_printer_print_str(p, array->type);
  else
  {
    p = isl_printer_print_str(p, array->name);
    p = isl_printer_print_str(p, "_t");
    p = isl_printer_print_int(p, n_lane);
  }
  p = isl_printer_print_str(p, " ");
  p = isl_printer_print_str(p, "*");
  p = isl_printer_print_str(p, array->name);
  if (n_ref >= 0)
  {
    p = isl_printer_print_str(p, "_");
    p = isl_printer_print_int(p, n_ref);
  }

  return p;
}

/* Print the arguments to a kernel declaration or call.  If "types" is set,
 * then print a declaration (including the types of the arguments).
 *
 * The arguments are printed in the following order
 * - the arrays accessed by the kernel
 * - the parameters
 * - the host loop iterators
 */
__isl_give isl_printer *print_kernel_arguments(__isl_take isl_printer *p,
                                               struct autosa_prog *prog, struct autosa_kernel *kernel,
                                               int types, struct hls_info *hls)
{
  int i, n;
  int first = 1;
  unsigned nparam;
  isl_space *space;
  const char *type;

  /* Arrays */
  for (i = 0; i < kernel->n_array; ++i)
  {
    int required;
    int n_lane;

    required = autosa_kernel_requires_array_argument(kernel, i);
    if (required < 0)
      return isl_printer_free(p);
    if (!required)
      continue;

    struct autosa_local_array_info *local_array = &kernel->array[i];
    n_lane = local_array->n_lane;
    if (hls->target == INTEL_HW ||
        (hls->target == XILINX_HW && local_array->n_io_group_refs == 1))
    {
      if (!first)
        p = isl_printer_print_str(p, ", ");

      if (types)
        p = autosa_array_info_print_declaration_argument(p,
                                                         local_array->array, n_lane, NULL, -1);
      else
        p = autosa_array_info_print_call_argument(p,
                                                  local_array->array, 0);

      first = 0;
    }
    else
    {
      for (int j = 0; j < local_array->n_io_group_refs; j++)
      {
        if (!first)
          p = isl_printer_print_str(p, ", ");

        if (types)
          p = autosa_array_info_print_declaration_argument(p,
                                                           local_array->array, n_lane, NULL, j);
        else
        {
          p = autosa_array_info_print_call_argument(p,
                                                    local_array->array, j);
        }

        first = 0;
      }
    }
  }

  /* Parameters */
  space = isl_union_set_get_space(kernel->arrays);
  nparam = isl_space_dim(space, isl_dim_param);
  for (i = 0; i < nparam; ++i)
  {
    const char *name;

    name = isl_space_get_dim_name(space, isl_dim_param, i);

    if (!first)
      p = isl_printer_print_str(p, ", ");
    if (types)
      p = isl_printer_print_str(p, "int ");
    p = isl_printer_print_str(p, name);

    first = 0;
  }
  isl_space_free(space);

  /* Host loop iterators */
  n = isl_space_dim(kernel->space, isl_dim_set);
  type = isl_options_get_ast_iterator_type(prog->ctx);
  for (i = 0; i < n; ++i)
  {
    const char *name;

    if (!first)
      p = isl_printer_print_str(p, ", ");
    name = isl_space_get_dim_name(kernel->space, isl_dim_set, i);
    if (types)
    {
      p = isl_printer_print_str(p, type);
      p = isl_printer_print_str(p, " ");
    }
    p = isl_printer_print_str(p, name);

    first = 0;
  }

  return p;
}

/* Print the header of the given kernel.
 */
__isl_give isl_printer *print_kernel_header(__isl_take isl_printer *p,
                                            struct autosa_prog *prog, struct autosa_kernel *kernel, struct hls_info *hls)
{
  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "void kernel");
  p = isl_printer_print_int(p, kernel->id);
  p = isl_printer_print_str(p, "(");
  p = print_kernel_arguments(p, prog, kernel, 1, hls);
  p = isl_printer_print_str(p, ")");

  return p;
}

/* This function is called for each node in a AutoSA AST.
 * In case of a user node, print the macro definitions required
 * for printing the AST expressions in the annotation, if any.
 * For other nodes, return true such that descendants are also
 * visited.
 *
 * In particular, for a kernel launch, print the macro definitions
 * needed for the grid size.
 * For a copy statement, print the macro definitions needed
 * for the two index expressions.
 * For an original user statement, print the macro definitions
 * needed for the substitutions.
 */
static isl_bool at_node(__isl_keep isl_ast_node *node, void *user)
{
  const char *name;
  isl_id *id;
  int is_kernel;
  struct autosa_kernel *kernel;
  struct autosa_kernel_stmt *stmt;
  isl_printer **p = (isl_printer **)user;

  if (isl_ast_node_get_type(node) != isl_ast_node_user)
    return isl_bool_true;

  id = isl_ast_node_get_annotation(node);
  if (!id)
    return isl_bool_false;

  name = isl_id_get_name(id);
  if (!name)
    return isl_bool_error;
  is_kernel = !strcmp(name, "kernel");
  kernel = is_kernel ? (struct autosa_kernel *)isl_id_get_user(id) : NULL;
  stmt = is_kernel ? NULL : (struct autosa_kernel_stmt *)isl_id_get_user(id);
  isl_id_free(id);

  if ((is_kernel && !kernel) || (!is_kernel && !stmt))
    return isl_bool_error;

  if (is_kernel)
  {
    *p = ppcg_ast_expr_print_macros(kernel->grid_size_expr, *p);
  }
  else if (stmt->type == AUTOSA_KERNEL_STMT_COPY)
  {
    *p = ppcg_ast_expr_print_macros(stmt->u.c.index, *p);
    *p = ppcg_ast_expr_print_macros(stmt->u.c.local_index, *p);
  }
  else if (stmt->type == AUTOSA_KERNEL_STMT_DOMAIN)
  {
    *p = ppcg_print_body_macros(*p, stmt->u.d.ref2expr);
  }
  if (!*p)
    return isl_bool_error;

  return isl_bool_false;
}

static void print_indent(FILE *dst, int indent)
{
  fprintf(dst, "%*s", indent, "");
}

/* Print a list of iterators of type "type" with names "ids" to "out".
 * Each iterator is assigned one of the instance identifiers in dims.
 */
static void print_iterators(FILE *out, const char *type,
                            __isl_keep isl_id_list *ids, const char *dims[])
{
  int i, n;

  n = isl_id_list_n_id(ids);
  if (n <= 0)
    return;
  print_indent(out, 4);
  fprintf(out, "%s ", type);
  for (i = 0; i < n; ++i)
  {
    isl_id *id;

    if (i)
      fprintf(out, ", ");
    id = isl_id_list_get_id(ids, i);
    fprintf(out, "%s = %s", isl_id_get_name(id),
            dims[i]);
    isl_id_free(id);
  }
  fprintf(out, "; // module id\n");
}

/* Print the required macros for the AutoSA AST "node" to "p",
 * including those needed for the user statements inside the AST.
 */
__isl_give isl_printer *autosa_print_macros(__isl_take isl_printer *p,
                                            __isl_keep isl_ast_node *node)
{
  if (isl_ast_node_foreach_descendant_top_down(node, &at_node, &p) < 0)
    return isl_printer_free(p);
  p = ppcg_print_macros(p, node);
  return p;
}

void print_module_iterators(FILE *out, struct autosa_hw_module *module)
{
  isl_ctx *ctx;
  const char *type;
  const char *dims[] = {"idx", "idy", "idz"};

  ctx = isl_ast_node_get_ctx(module->tree);
  type = isl_options_get_ast_iterator_type(ctx);
  print_iterators(out, type, module->inst_ids, dims);
}

void print_func_iterators(FILE *out,
                          struct autosa_drain_merge_func *func)
{
  isl_ctx *ctx;
  const char *type;
  const char *dims[] = {"idx", "idy", "idz"};

  ctx = isl_ast_node_get_ctx(func->tree);
  type = isl_options_get_ast_iterator_type(ctx);
  print_iterators(out, type, func->inst_ids, dims);
}

/* Print out
 * "hls::stream<[type]>"
 */
__isl_give isl_printer *print_fifo_type_xilinx(__isl_take isl_printer *p,
                                               struct autosa_array_ref_group *group, int n_lane)
{
  p = isl_printer_print_str(p, "hls::stream<");
  if (n_lane == 1)
  {
    p = isl_printer_print_str(p, group->array->type);
  }
  else
  {
    struct autosa_array_info *array = group->array;
    p = isl_printer_print_str(p, array->name);
    p = isl_printer_print_str(p, "_t");
    p = isl_printer_print_int(p, n_lane);
  }
  p = isl_printer_print_str(p, ">");

  return p;
}

/* Print out
 * "channel [type]"
 */
__isl_give isl_printer *print_fifo_type_intel(__isl_take isl_printer *p,
                                              struct autosa_array_ref_group *group, int n_lane)
{
  p = isl_printer_print_str(p, "channel ");
  if (n_lane == 1)
    p = isl_printer_print_str(p, group->array->type);
  else
  {
    p = isl_printer_print_str(p, group->array->name);
    p = isl_printer_print_str(p, "_t");
    p = isl_printer_print_int(p, n_lane);
  }

  return p;
}

__isl_give isl_printer *autosa_fifo_print_declaration_arguments(
    __isl_take isl_printer *p, struct autosa_array_ref_group *group, int n_lane,
    const char *suffix, enum platform target)
{
  if (target == XILINX_HW)
  {
    p = print_fifo_type_xilinx(p, group, n_lane);
    p = isl_printer_print_str(p, " &");
  }
  else
  {
    p = print_fifo_type_intel(p, group, n_lane);
    p = isl_printer_print_str(p, " ");
  }
  p = autosa_array_ref_group_print_fifo_name(group, p);
  if (suffix)
  {
    p = isl_printer_print_str(p, "_");
    p = isl_printer_print_str(p, suffix);
  }

  return p;
}

__isl_give isl_printer *autosa_fifo_print_call_argument(
    __isl_take isl_printer *p, struct autosa_array_ref_group *group,
    const char *suffix, enum platform target)
{
  p = autosa_array_ref_group_print_fifo_name(group, p);
  if (suffix)
  {
    p = isl_printer_print_str(p, "_");
    p = isl_printer_print_str(p, suffix);
  }

  return p;
}

/* Print the call of an array argument in the module.
 */
__isl_give isl_printer *autosa_module_array_info_print_call_argument(
    __isl_take isl_printer *p, struct autosa_array_info *array)
{
  if (autosa_array_is_read_only_scalar(array))
    return isl_printer_print_str(p, array->name);

  p = isl_printer_print_str(p, array->name);

  return p;
}

/* Print the arguments to a module declaration or call. If "types" is set,
 * then print a declaration (including the types of the arguments).
 *
 * The arguments are printed in the following order
 * - the module identifiers
 * - the parameters
 * - the host loop iterators
 * - the arrays accessed by the module
 * - the fifos
 * - the enable signal
 */
__isl_give isl_printer *print_module_arguments(
    __isl_take isl_printer *p,
    struct autosa_prog *prog,
    struct autosa_kernel *kernel,
    struct autosa_hw_module *module, int types,
    enum platform target,
    int inter, int arb, int boundary)
{
  int first = 1;
  isl_space *space;
  int nparam;
  int n;
  const char *type;

  type = isl_options_get_ast_iterator_type(prog->ctx);
  /* Module identifiers */
  const char *dims[] = {"idx", "idy", "idz"};
  n = isl_id_list_n_id(module->inst_ids);
  for (int i = 0; i < n; ++i)
  {
    if (!first)
      p = isl_printer_print_str(p, ", ");
    if (types)
    {
      p = isl_printer_print_str(p, type);
      p = isl_printer_print_str(p, " ");
    }
    p = isl_printer_print_str(p, dims[i]);

    first = 0;
  }

  /* params */
  space = isl_union_set_get_space(kernel->arrays);
  nparam = isl_space_dim(space, isl_dim_param);
  for (int i = 0; i < nparam; ++i)
  {
    const char *name;

    name = isl_space_get_dim_name(space, isl_dim_param, i);

    if (!first)
      p = isl_printer_print_str(p, ", ");
    if (types)
      p = isl_printer_print_str(p, "int ");
    p = isl_printer_print_str(p, name);

    first = 0;
  }
  isl_space_free(space);

  /* host iters */
  if (inter == -1)
    space = module->space;
  else if (inter == 0)
    space = module->intra_space;
  else if (inter == 1)
    space = module->inter_space;

  n = isl_space_dim(space, isl_dim_set);
  for (int i = 0; i < n; ++i)
  {
    const char *name;

    if (!first)
      p = isl_printer_print_str(p, ", ");
    name = isl_space_get_dim_name(space, isl_dim_set, i);
    if (types)
    {
      p = isl_printer_print_str(p, type);
      p = isl_printer_print_str(p, " ");
    }
    p = isl_printer_print_str(p, name);
    if (module->double_buffer && inter != -1)
    {
      if (module->in && inter == 0)
      {
        /* intra trans */
        p = isl_printer_print_str(p, "_prev");
      }
      else if (!module->in && inter == 1)
      {
        /* inter trans */
        p = isl_printer_print_str(p, "_prev");
      }
    }

    first = 0;
  }

  /* Arrays */
  if (module->type != PE_MODULE && module->to_mem)
  {
    /* I/O module that accesses the external memory. */
    struct autosa_io_buffer *io_buffer =
        module->io_groups[0]->io_buffers[module->io_groups[0]->io_level - 1];
    int n_lane = io_buffer->n_lane;
    if (!first)
    {
      p = isl_printer_print_str(p, ", ");
    }
    if (types)
    {
      p = autosa_array_info_print_declaration_argument(p,
                                                       module->io_groups[0]->array, n_lane,
                                                       target == INTEL_HW ? "global" : NULL, -1);
    }
    else
    {
      p = autosa_module_array_info_print_call_argument(p,
                                                       module->io_groups[0]->array);
    }
    first = 0;
  }
  else if (module->type == PE_MODULE)
  {
    /* Scalars */
    for (int i = 0; i < prog->n_array; i++)
    {
      int required;

      required = autosa_kernel_requires_array_argument(kernel, i);
      if (required < 0)
        return isl_printer_free(p);
      if (!required)
        continue;

      if (autosa_array_is_read_only_scalar(&prog->array[i]))
      {
        if (!first)
        {
          p = isl_printer_print_str(p, ", ");
        }
        if (types)
          p = autosa_array_info_print_declaration_argument(p,
                                                           &prog->array[i], 1, NULL, -1);
        else
          p = autosa_array_info_print_call_argument(p,
                                                    &prog->array[i], -1);
        first = 0;
      }
    }
  }

  /* Local buffer */
  if (inter != -1)
  {
    for (int i = 0; i < module->n_var; i++)
    {
      struct autosa_kernel_var *var;

      var = (struct autosa_kernel_var *)&module->var[i];
      if (!first)
        p = isl_printer_print_str(p, ", ");

      if (types)
      {
        if (module->data_pack_inter == 1)
        {
          p = isl_printer_print_str(p, var->array->type);
        }
        else
        {
          p = isl_printer_print_str(p, var->array->name);
          p = isl_printer_print_str(p, "_t");
          p = isl_printer_print_int(p, module->data_pack_inter);
        }
        p = isl_printer_print_str(p, " ");
        p = isl_printer_print_str(p, var->name);
        for (int j = 0; j < isl_vec_size(var->size); j++)
        {
          isl_val *v;

          p = isl_printer_print_str(p, "[");
          v = isl_vec_get_element_val(var->size, j);
          p = isl_printer_print_val(p, v);
          isl_val_free(v);
          p = isl_printer_print_str(p, "]");
        }
      }
      else
      {
        if (!module->double_buffer)
        {
          p = isl_printer_print_str(p, var->name);
        }
        else
        {
          if (arb == 0)
          {
            p = isl_printer_print_str(p, var->name);
            p = isl_printer_print_str(p, inter == 0 ? "_ping" : "_pong");
          }
          else
          {
            p = isl_printer_print_str(p, var->name);
            p = isl_printer_print_str(p, inter == 0 ? "_pong" : "_ping");
          }
        }
      }

      first = 0;
    }
  }

  /* fifos */
  if (module->type == PE_MODULE)
  {
    for (int i = 0; i < module->n_io_group; i++)
    {
      struct autosa_array_ref_group *group = module->io_groups[i];
      int n_lane = get_io_group_n_lane(module, group);
      if (module->io_groups[i]->pe_io_dir == IO_IN ||
          module->io_groups[i]->pe_io_dir == IO_INOUT)
      {
        if (!first)
        {
          p = isl_printer_print_str(p, ", ");
        }
        if (types)
        {
          p = autosa_fifo_print_declaration_arguments(p,
                                                      module->io_groups[i], n_lane, "in", target);
        }
        else
          p = autosa_fifo_print_call_argument(p,
                                              module->io_groups[i], "in", target);
        first = 0;
      }
      if (module->io_groups[i]->pe_io_dir == IO_OUT ||
          module->io_groups[i]->pe_io_dir == IO_INOUT)
      {
        if (!first)
          p = isl_printer_print_str(p, ", ");
        if (types)
          p = autosa_fifo_print_declaration_arguments(p,
                                                      module->io_groups[i], n_lane, "out", target);
        else
          p = autosa_fifo_print_call_argument(p,
                                              module->io_groups[i], "out", target);
        first = 0;
      }
    }
  }
  else
  {
    for (int i = 0; i < module->n_io_group; i++)
    {
      if (!module->to_mem && inter != 0)
      {
        if (!(!module->in && boundary))
        {
          if (!first)
          {
            p = isl_printer_print_str(p, ", ");
          }
          /* in */
          if (types)
            p = autosa_fifo_print_declaration_arguments(p,
                                                        module->io_groups[i], module->data_pack_inter, "in", target);
          else
            p = autosa_fifo_print_call_argument(p,
                                                module->io_groups[i], "in", target);
          first = 0;
        }

        if (!(module->in && boundary))
        {
          /* out */
          if (!first)
            p = isl_printer_print_str(p, ", ");
          if (types)
            p = autosa_fifo_print_declaration_arguments(p,
                                                        module->io_groups[i], module->data_pack_inter, "out", target);
          else
            p = autosa_fifo_print_call_argument(p,
                                                module->io_groups[i], "out", target);
          first = 0;
        }
      }

      if (inter != 1)
      {
        if (!first)
          p = isl_printer_print_str(p, ", ");
        /* local */
        if (types)
          p = autosa_fifo_print_declaration_arguments(p,
                                                      module->io_groups[i], module->data_pack_intra,
                                                      module->in ? "local_out" : "local_in", target);
        else
          p = autosa_fifo_print_call_argument(p,
                                              module->io_groups[i], module->in ? "local_out" : "local_in", target);
        first = 0;
      }
    }
  }

  /* credit fifo */
  if (module->credit)
  {
    if (!first)
    {
      p = isl_printer_print_str(p, ", ");
    }
    if (types)
    {
      if (target == XILINX_HW)
      {
        p = isl_printer_print_str(p, "hls::stream<int> &credit");
      }
      else
      {
        p = isl_printer_print_str(p, "channel int credit");
      }
    }
    else
    {
      p = isl_printer_print_str(p, "credit");
    }

    first = 0;
  }

  /* enable signal */
  if (module->double_buffer && inter != -1)
  {
    if (!first)
    {
      p = isl_printer_print_str(p, ", ");
    }
    if (types)
    {
      p = isl_printer_print_str(p, inter == 0 ? "bool intra_trans_en" : "bool inter_trans_en");
    }
    else
    {
      p = isl_printer_print_str(p, inter == 0 ? "intra_trans_en" : "inter_trans_en");
    }

    first = 0;
  }

  return p;
}

/* Print the arguments to a pe dummy module declaration or call. If "types" is set,
 * then print a declaration (including the types of the arguments).
 *
 * The arguments are printed in the following order
 * - the module identifiers
 * - the parameters
 * - the host loop iterators 
 * - the arrays accessed by the module
 * - the fifos
 */
__isl_give isl_printer *print_pe_dummy_module_arguments(
    __isl_take isl_printer *p,
    struct autosa_prog *prog,
    struct autosa_kernel *kernel,
    struct autosa_pe_dummy_module *pe_dummy_module,
    int types,
    enum platform target)
{
  int first = 1;
  isl_space *space;
  int nparam;
  int n;
  const char *type;
  struct autosa_hw_module *module = pe_dummy_module->module;

  type = isl_options_get_ast_iterator_type(prog->ctx);
  /* module identifiers */
  const char *dims[] = {"idx", "idy", "idz"};
  n = isl_id_list_n_id(module->inst_ids);
  for (int i = 0; i < n; ++i)
  {
    if (!first)
      p = isl_printer_print_str(p, ", ");
    if (types)
    {
      p = isl_printer_print_str(p, type);
      p = isl_printer_print_str(p, " ");
    }
    p = isl_printer_print_str(p, dims[i]);

    first = 0;
  }

  /* params */
  space = isl_union_set_get_space(kernel->arrays);
  nparam = isl_space_dim(space, isl_dim_param);
  for (int i = 0; i < nparam; ++i)
  {
    const char *name;

    name = isl_space_get_dim_name(space, isl_dim_param, i);

    if (!first)
      p = isl_printer_print_str(p, ", ");
    if (types)
      p = isl_printer_print_str(p, "int ");
    p = isl_printer_print_str(p, name);

    first = 0;
  }
  isl_space_free(space);

  /* host iters */
  space = module->space;

  n = isl_space_dim(space, isl_dim_set);
  for (int i = 0; i < n; ++i)
  {
    const char *name;

    if (!first)
      p = isl_printer_print_str(p, ", ");
    name = isl_space_get_dim_name(space, isl_dim_set, i);
    if (types)
    {
      p = isl_printer_print_str(p, type);
      p = isl_printer_print_str(p, " ");
    }
    p = isl_printer_print_str(p, name);

    first = 0;
  }

  /* Arrays */
  /* Scalars */
  for (int i = 0; i < prog->n_array; i++)
  {
    int required;

    required = autosa_kernel_requires_array_argument(kernel, i);
    if (required < 0)
      return isl_printer_free(p);
    if (!required)
      continue;

    if (autosa_array_is_read_only_scalar(&prog->array[i]))
    {
      if (!first)
      {
        p = isl_printer_print_str(p, ", ");
      }
      if (types)
        p = autosa_array_info_print_declaration_argument(p,
                                                         &prog->array[i], 1, NULL, -1);
      else
        p = autosa_module_array_info_print_call_argument(p,
                                                         &prog->array[i]);
      first = 0;
    }
  }

  /* fifos */
  struct autosa_array_ref_group *group = pe_dummy_module->io_group;
  int n_lane = (group->local_array->array_type == AUTOSA_EXT_ARRAY) ? group->n_lane : ((group->group_type == AUTOSA_DRAIN_GROUP) ? group->n_lane : ((group->io_type == AUTOSA_EXT_IO) ? group->n_lane : group->io_buffers[0]->n_lane));

  if (!first)
  {
    p = isl_printer_print_str(p, ", ");
  }
  if (types)
  {
    p = autosa_fifo_print_declaration_arguments(p,
                                                group, n_lane, "in", target);
  }
  else
    p = autosa_fifo_print_call_argument(p,
                                        group, "in", target);
  first = 0;

  return p;
}

/* Print the arguments of the top_gen function:
 * - parameters
 * - host loop iterators
 * - file descriptor
 */
__isl_give isl_printer *print_top_gen_arguments(__isl_take isl_printer *p,
                                                struct autosa_prog *prog, struct autosa_kernel *kernel, int types)
{
  int i, n;
  int first = 1;
  unsigned nparam;
  isl_space *space;
  const char *type;

  /* Parameters */
  space = isl_union_set_get_space(kernel->arrays);
  nparam = isl_space_dim(space, isl_dim_param);
  for (i = 0; i < nparam; ++i)
  {
    const char *name;

    name = isl_space_get_dim_name(space, isl_dim_param, i);

    if (!first)
      p = isl_printer_print_str(p, ", ");
    if (types)
      p = isl_printer_print_str(p, "int ");
    p = isl_printer_print_str(p, name);

    first = 0;
  }
  isl_space_free(space);

  /* Host iterators */
  n = isl_space_dim(kernel->space, isl_dim_set);
  type = isl_options_get_ast_iterator_type(prog->ctx);
  for (i = 0; i < n; ++i)
  {
    const char *name;

    if (!first)
      p = isl_printer_print_str(p, ", ");
    name = isl_space_get_dim_name(kernel->space, isl_dim_set, i);
    if (types)
    {
      p = isl_printer_print_str(p, type);
      p = isl_printer_print_str(p, " ");
    }
    p = isl_printer_print_str(p, name);

    first = 0;
  }

  /* File description */
  if (!first)
    p = isl_printer_print_str(p, ", ");
  if (types)
  {
    p = isl_printer_print_str(p, "FILE *");
  }
  p = isl_printer_print_str(p, "f");

  first = 0;

  return p;
}

static __isl_give isl_printer *print_top_gen_header(__isl_take isl_printer *p,
                                                    struct autosa_prog *prog, struct autosa_hw_top_module *top)
{
  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "void ");
  p = isl_printer_print_str(p, "top_generate");
  p = isl_printer_print_str(p, "(");
  p = print_top_gen_arguments(p, prog, top->kernel, 1);
  p = isl_printer_print_str(p, ")");

  return p;
}

void print_top_gen_headers(
    struct autosa_prog *prog, struct autosa_hw_top_module *top, struct hls_info *hls)
{
  isl_printer *p;

  p = isl_printer_to_file(prog->ctx, hls->top_gen_h);
  p = isl_printer_set_output_format(p, ISL_FORMAT_C);
  p = print_top_gen_header(p, prog, top);
  p = isl_printer_print_str(p, ";");
  p = isl_printer_end_line(p);
  isl_printer_free(p);

  p = isl_printer_to_file(prog->ctx, hls->top_gen_c);
  p = isl_printer_set_output_format(p, ISL_FORMAT_C);
  p = print_top_gen_header(p, prog, top);
  p = isl_printer_end_line(p);
  isl_printer_free(p);
}

/* Print out
 * "\/* [module_name] FIFO *\/"
 */
static __isl_give isl_printer *print_fifo_comment(
    __isl_take isl_printer *p, struct autosa_hw_module *module)
{
  p = isl_printer_print_str(p, "/* ");
  p = isl_printer_print_str(p, module->name);
  p = isl_printer_print_str(p, " fifo */");

  return p;
}

/* Print out
 * "_[c0 + val]"
 * Increase the "pos"th index by the value of "val"
 */
static __isl_give isl_printer *print_inst_ids_inc_suffix(
    __isl_take isl_printer *p, int n, int pos, int val)
{
  for (int i = 0; i < n; i++)
  {
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"_\");");
    p = isl_printer_end_line(p);
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_print_int(p, c");
    p = isl_printer_print_int(p, i);
    if (i == pos)
    {
      if (val != 0)
      {
        p = isl_printer_print_str(p, " + ");
        p = isl_printer_print_int(p, val);
      }
    }
    p = isl_printer_print_str(p, ");");
    p = isl_printer_end_line(p);
  }

  return p;
}

/* Print out
 * "_c0_c1"
 */
static __isl_give isl_printer *print_inst_ids_suffix(
    __isl_take isl_printer *p, int n, __isl_keep isl_vec *offset)
{
  for (int i = 0; i < n; i++)
  {
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"_\");");
    p = isl_printer_end_line(p);
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_print_int(p, c");
    p = isl_printer_print_int(p, i);
    if (offset)
    {
      isl_val *val = isl_vec_get_element_val(offset, i);
      if (!isl_val_is_zero(val))
      {
        p = isl_printer_print_str(p, " + ");
        p = isl_printer_print_val(p, val);
      }
      isl_val_free(val);
    }
    p = isl_printer_print_str(p, ");");
    p = isl_printer_end_line(p);
  }

  return p;
}

/* This function prints the inst ids described by "expr".
 * If the "offset" is set, it is added to the inst ids.
 */
static __isl_give isl_printer *print_pretrans_inst_ids_suffix(
    __isl_take isl_printer *p, int n_id,
    __isl_keep isl_ast_expr *expr, __isl_keep isl_vec *offset)
{
  isl_ctx *ctx = isl_ast_expr_get_ctx(expr);
  int n;

  n = isl_ast_expr_op_get_n_arg(expr);
  for (int i = 0; i < n_id; i++)
  {
    isl_ast_expr *expr_i = isl_ast_expr_get_op_arg(expr, i + 1);
    int format;

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"_\");");
    p = isl_printer_end_line(p);

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_print_int(p, ");
    format = isl_printer_get_output_format(p);
    p = isl_printer_set_output_format(p, ISL_FORMAT_C);
    p = isl_printer_print_ast_expr(p, expr_i);
    p = isl_printer_set_output_format(p, format);
    if (offset)
    {
      isl_val *val = isl_vec_get_element_val(offset, i);
      if (!isl_val_is_zero(val))
      {
        p = isl_printer_print_str(p, " + ");
        p = isl_printer_print_val(p, val);
      }
      isl_val_free(val);
    }
    p = isl_printer_print_str(p, ");");
    p = isl_printer_end_line(p);

    isl_ast_expr_free(expr_i);
  }

  return p;
}

static __isl_give isl_printer *print_fifo_decl_single(
    __isl_take isl_printer *p,
    struct autosa_kernel_stmt *stmt, struct autosa_prog *prog,
    struct hls_info *hls, int pe_inout, const char *suffix)
{
  struct autosa_hw_module *module = stmt->u.m.module;
  struct autosa_array_ref_group *group = stmt->u.m.group;
  int boundary = stmt->u.m.boundary;
  int n;
  int n_lane;

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "// Count channel number");
  p = isl_printer_end_line(p);

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "fifo_cnt++;");
  p = isl_printer_end_line(p);

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "// Print channel declarations of module: ");
  p = isl_printer_print_str(p, module->name);
  p = isl_printer_end_line(p);

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "p = isl_printer_start_line(p);");
  p = isl_printer_end_line(p);

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"");
  p = print_fifo_comment(p, module);
  p = isl_printer_print_str(p, " ");
  n_lane = get_io_group_n_lane(module, group);
  if (hls->target == XILINX_HW)
    p = print_fifo_type_xilinx(p, group, n_lane);
  else if (hls->target == INTEL_HW)
    p = print_fifo_type_intel(p, group, n_lane);
  p = isl_printer_print_str(p, " ");
  p = autosa_array_ref_group_print_fifo_name(group, p);
  p = isl_printer_print_str(p, "_");
  p = isl_printer_print_str(p, module->name);
  if (pe_inout)
  {
    p = isl_printer_print_str(p, suffix);
  }
  p = isl_printer_print_str(p, "\");");
  p = isl_printer_end_line(p);

  n = isl_id_list_n_id(module->inst_ids);
  if (module->type == IO_MODULE || module->type == DRAIN_MODULE)
  {
    if (boundary)
    {
      p = print_inst_ids_inc_suffix(p, n, n - 1, 1);
    }
    else
    {
      p = print_inst_ids_suffix(p, n, NULL);
    }
  }
  else if (module->type == PE_MODULE)
  {
    if (boundary)
      p = print_pretrans_inst_ids_suffix(p, n, group->io_L1_pe_expr, group->dir);
    else
      p = print_pretrans_inst_ids_suffix(p, n, group->io_L1_pe_expr, NULL);
  }
  if (hls->target == INTEL_HW)
  {
    /* Print fifo attribute */
    p = print_str_new_line(p, "p = isl_printer_print_str(p, \" __attribute__((depth(2)))\");");
  }
  //p = isl_printer_start_line(p);
  //p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \";\");");
  //p = isl_printer_end_line(p);
  p = print_str_new_line(p, "p = isl_printer_print_str(p, \";\");");

  //p = isl_printer_start_line(p);
  //p = isl_printer_print_str(p, "p = isl_printer_end_line(p);");
  //p = isl_printer_end_line(p);
  p = print_str_new_line(p, "p = isl_printer_end_line(p);");

  if (hls->target == XILINX_HW)
  {
    /* Print fifo pragma */
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_start_line(p);");
    p = isl_printer_end_line(p);

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"#pragma HLS STREAM variable=");
    p = autosa_array_ref_group_print_fifo_name(group, p);
    p = isl_printer_print_str(p, "_");
    p = isl_printer_print_str(p, module->name);
    if (pe_inout)
    {
      p = isl_printer_print_str(p, suffix);
    }
    p = isl_printer_print_str(p, "\");");
    p = isl_printer_end_line(p);

    if (module->type == IO_MODULE || module->type == DRAIN_MODULE)
    {
      if (boundary)
      {
        p = print_inst_ids_inc_suffix(p, n, n - 1, 1);
      }
      else
      {
        p = print_inst_ids_suffix(p, n, NULL);
      }
    }
    else if (module->type == PE_MODULE)
    {
      if (boundary)
      {
        p = print_pretrans_inst_ids_suffix(p, n, group->io_L1_pe_expr, group->dir);
      }
      else
      {
        p = print_pretrans_inst_ids_suffix(p, n, group->io_L1_pe_expr, NULL);
      }
    }
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \" depth=2\");");
    p = isl_printer_end_line(p);

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_end_line(p);");
    p = isl_printer_end_line(p);

    /* If depth * width > 512 bits, HLS will use BRAM to implement FIFOs.
     * Instead, we will insert pragmas to use SRL instead.
     */
    /* Print fifo resource pragma. */
    if (n_lane * group->array->size > 32)
    {
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "p = isl_printer_start_line(p);");
      p = isl_printer_end_line(p);

      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"#pragma HLS RESOURCE variable=");
      p = autosa_array_ref_group_print_fifo_name(group, p);
      p = isl_printer_print_str(p, "_");
      p = isl_printer_print_str(p, module->name);
      if (pe_inout)
      {
        p = isl_printer_print_str(p, suffix);
      }
      p = isl_printer_print_str(p, "\");");
      p = isl_printer_end_line(p);

      if (module->type == IO_MODULE || module->type == DRAIN_MODULE)
      {
        if (boundary)
        {
          p = print_inst_ids_inc_suffix(p, n, n - 1, 1);
        }
        else
        {
          p = print_inst_ids_suffix(p, n, NULL);
        }
      }
      else if (module->type == PE_MODULE)
      {
        if (boundary)
        {
          p = print_pretrans_inst_ids_suffix(p, n, group->io_L1_pe_expr, group->dir);
        }
        else
        {
          p = print_pretrans_inst_ids_suffix(p, n, group->io_L1_pe_expr, NULL);
        }
      }
      //p = isl_printer_start_line(p);
      //p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \" core=FIFO_SRL\");");
      //p = isl_printer_end_line(p);
      p = print_str_new_line(p, "p = isl_printer_print_str(p, \" core=FIFO_SRL\");");

      //p = isl_printer_start_line(p);
      //p = isl_printer_print_str(p, "p = isl_printer_end_line(p);");
      //p = isl_printer_end_line(p);
      p = print_str_new_line(p, "p = isl_printer_end_line(p);");
    }
  }

  return p;
}

/* if module->type == PE_MODULE
 *   if boundary == 0:
 *     new_inst_id = io_trans(inst_id)
 *     print [fifo_name]_[module_name]_[new_inst_id]
 *   else if boundary == 1:
 *     new_inst_id = io_trans(inst_id)
 *     print [fifo_name]_[module_name]_[new_inst_id + dep_dir]
 * if module->type == IO_MODULE:
 *     print [fifo_name]_[module_name]_[inst_id]
 */
static __isl_give isl_printer *print_fifo_decl(__isl_take isl_printer *p,
                                               struct autosa_kernel_stmt *stmt, struct autosa_prog *prog, struct hls_info *hls)
{
  struct autosa_hw_module *module = stmt->u.m.module;
  struct autosa_array_ref_group *group = stmt->u.m.group;
  int pe_inout;

  if (isl_vec_is_zero(group->old_dir) && module->type == PE_MODULE && group->pe_io_dir == IO_INOUT)
  {
    pe_inout = 1;
  }
  else
  {
    pe_inout = 0;
  }

  if (pe_inout)
  {
    p = print_fifo_decl_single(p, stmt, prog, hls, 1, "_in");
    p = print_fifo_decl_single(p, stmt, prog, hls, 1, "_out");
  }
  else
  {
    p = print_fifo_decl_single(p, stmt, prog, hls, 0, NULL);
  }

  return p;
}

__isl_give isl_printer *autosa_kernel_print_fifo_decl(
    __isl_take isl_printer *p,
    struct autosa_kernel_stmt *stmt, struct autosa_prog *prog, struct hls_info *hls)
{
  p = ppcg_start_block(p);

  /* Build the fifo_decl. */
  p = print_fifo_decl(p, stmt, prog, hls);

  p = ppcg_end_block(p);

  return p;
}

static __isl_give isl_printer *print_delimiter(__isl_take isl_printer *p,
                                               int *first)
{
  if (!(*first))
  {
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \",\");");
    p = isl_printer_end_line(p);

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_end_line(p);");
    p = isl_printer_end_line(p);
  }
  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "p = isl_printer_start_line(p);");
  p = isl_printer_end_line(p);

  *first = 0;

  return p;
}

static __isl_give isl_printer *print_fifo_annotation(__isl_take isl_printer *p,
                                                     struct autosa_hw_module *module,
                                                     struct autosa_array_ref_group *group, int in, int lower)
{
  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"/* fifo */ \");");
  p = isl_printer_end_line(p);

  return p;
}

/* Print out
 * [fifo_name]_[module_name]
 */
static __isl_give isl_printer *print_fifo_prefix(__isl_take isl_printer *p,
                                                 struct autosa_hw_module *module, struct autosa_array_ref_group *group)
{
  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"");
  p = autosa_array_ref_group_print_fifo_name(group, p);
  p = isl_printer_print_str(p, "_");
  p = isl_printer_print_str(p, module->name);
  p = isl_printer_print_str(p, "\");");
  p = isl_printer_end_line(p);

  return p;
}

/* Print the upper body of the module call, including:
 * - module identifier
 * - parameters
 * - host loop iterators
 * - arrays
 * - inter-module fifos
 */
__isl_give isl_printer *print_module_call_upper(__isl_take isl_printer *p,
                                                struct autosa_kernel_stmt *stmt, struct autosa_prog *prog,
                                                enum platform target)
{
  struct autosa_hw_module *module = stmt->u.m.module;
  struct autosa_pe_dummy_module *pe_dummy_module = stmt->u.m.pe_dummy_module;
  int lower = stmt->u.m.lower;
  int upper = stmt->u.m.upper;
  int boundary = stmt->u.m.boundary;
  int dummy = stmt->u.m.dummy;
  int first = 1;
  int n;
  char *module_name = stmt->u.m.module_name;
  isl_space *space;

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "// Print calls of module: ");
  p = isl_printer_print_str(p, module_name);
  if (boundary)
  {
    p = isl_printer_print_str(p, "_boundary");
  }
  p = isl_printer_end_line(p);

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "p = isl_printer_start_line(p);");
  p = isl_printer_end_line(p);

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"");
  p = isl_printer_print_str(p, module_name);
  if (boundary)
  {
    p = isl_printer_print_str(p, "_boundary");
  }
  if (target == XILINX_HW)
    p = isl_printer_print_str(p, "_wrapper");
  p = isl_printer_print_str(p, "(\");");
  p = isl_printer_end_line(p);

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "p = isl_printer_end_line(p);");
  p = isl_printer_end_line(p);

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "p = isl_printer_indent(p, 4);");
  p = isl_printer_end_line(p);

  /* module identifiers */
  if (!dummy)
  {
    for (int i = 0; i < isl_id_list_n_id(module->inst_ids); i++)
    {
      p = print_delimiter(p, &first);
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"/* module id */ \");");
      p = isl_printer_end_line(p);
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "p = isl_printer_print_int(p, c");
      p = isl_printer_print_int(p, i);
      p = isl_printer_print_str(p, ");");
      p = isl_printer_end_line(p);
    }
  }
  else
  {
    isl_ast_expr *expr = pe_dummy_module->io_group->io_L1_pe_expr;
    int n_arg = isl_ast_expr_op_get_n_arg(expr);

    for (int i = 0; i < isl_id_list_n_id(module->inst_ids); i++)
    {
      int format;
      p = print_delimiter(p, &first);
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"/* module id */ \");");
      p = isl_printer_end_line(p);
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "p = isl_printer_print_int(p, ");

      isl_ast_expr *expr_i = isl_ast_expr_get_op_arg(expr, i + 1);
      p = isl_printer_print_ast_expr(p, expr_i);
      isl_ast_expr_free(expr_i);

      p = isl_printer_print_str(p, ");");
      p = isl_printer_end_line(p);
    }
  }

  /* params */
  space = isl_union_set_get_space(module->kernel->arrays);
  n = isl_space_dim(space, isl_dim_param);
  for (int i = 0; i < n; i++)
  {
    p = print_delimiter(p, &first);

    const char *name = isl_space_get_dim_name(space, isl_dim_set, i);
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"/* param */");
    p = isl_printer_print_str(p, name);
    p = isl_printer_print_str(p, "\");");
    p = isl_printer_end_line(p);
  }
  isl_space_free(space);

  /* host iterators */
  n = isl_space_dim(module->kernel->space, isl_dim_set);
  for (int i = 0; i < n; i++)
  {
    p = print_delimiter(p, &first);

    const char *name = isl_space_get_dim_name(module->kernel->space, isl_dim_set, i);
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"/* host iter */ ");
    p = isl_printer_print_str(p, name);
    p = isl_printer_print_str(p, "\");");
    p = isl_printer_end_line(p);
  }

  /* scalar and arrays */
  if (module->type != PE_MODULE && module->to_mem)
  {
    p = print_delimiter(p, &first);

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"/* array */ ");
    p = isl_printer_print_str(p, module->io_groups[0]->array->name);
    if (module->io_groups[0]->local_array->n_io_group_refs > 1)
    {
      if (module->io_groups[0]->n_mem_ports == 1)
      {
        /* Print A_[module_n_array_ref] */
        p = isl_printer_print_str(p, "_");
        p = isl_printer_print_int(p, module->n_array_ref);
        p = isl_printer_print_str(p, "\");");
        p = isl_printer_end_line(p);
      }
      else
      {
        /* Print A_[module_n_array_ref + c0] */
        p = isl_printer_print_str(p, "_\");");
        p = isl_printer_end_line(p);
        p = isl_printer_start_line(p);
        p = isl_printer_print_str(p, "p = isl_printer_print_int(p, c0 + ");
        p = isl_printer_print_int(p, module->n_array_ref);
        p = isl_printer_print_str(p, ");");
        p = isl_printer_end_line(p);
      }
    }
    else
    {
      p = isl_printer_print_str(p, "\");");
      p = isl_printer_end_line(p);
    }
  }
  else if (module->type == PE_MODULE)
  {
    for (int i = 0; i < prog->n_array; i++)
    {
      int required;

      required = autosa_kernel_requires_array_argument(module->kernel, i);
      if (required < 0)
        return isl_printer_free(p);
      if (!required)
        continue;

      if (autosa_array_is_read_only_scalar(&prog->array[i]))
      {
        p = print_delimiter(p, &first);

        p = isl_printer_start_line(p);
        p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"/* scalar */ ");
        p = isl_printer_print_str(p, module->io_groups[0]->array->name);
        p = isl_printer_print_str(p, "\");");
        p = isl_printer_end_line(p);
      }
    }
  }

  /* FIFO */
  n = isl_id_list_n_id(module->inst_ids);
  if (module->type == PE_MODULE)
  {
    if (dummy)
    {
      struct autosa_array_ref_group *group = pe_dummy_module->io_group;
      p = print_delimiter(p, &first);
      p = print_fifo_annotation(p, module, group, 1, 0);
      p = print_fifo_prefix(p, module, group);
      if (isl_vec_is_zero(group->dir))
      {
        p = isl_printer_start_line(p);
        p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"_in\")");
        p = isl_printer_end_line(p);
      }
      p = print_pretrans_inst_ids_suffix(p, n, group->io_L1_pe_expr, group->dir);
    }
    else
    {
      for (int i = 0; i < module->n_io_group; i++)
      {
        struct autosa_array_ref_group *group = module->io_groups[i];
        if (group->pe_io_dir == IO_INOUT)
        {
          p = print_delimiter(p, &first);
          p = print_fifo_annotation(p, module, group, 1, 0);
          p = print_fifo_prefix(p, module, group);
          if (isl_vec_is_zero(group->old_dir))
          {
            p = isl_printer_start_line(p);
            p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"_in\");");
            p = isl_printer_end_line(p);
          }
          p = print_inst_ids_suffix(p, n, NULL);

          p = print_delimiter(p, &first);
          p = print_fifo_annotation(p, module, group, 0, 0);
          p = print_fifo_prefix(p, module, group);
          if (isl_vec_is_zero(group->old_dir))
          {
            p = isl_printer_start_line(p);
            p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"_out\");");
            p = isl_printer_end_line(p);
          }
          if (isl_vec_is_zero(group->old_dir))
          {
            p = print_inst_ids_suffix(p, n, NULL);
          }
          else
          {
            p = print_inst_ids_suffix(p, n, group->dir);
          }
        }
        else
        {
          p = print_delimiter(p, &first);
          p = print_fifo_annotation(p, module, group, group->pe_io_dir == IO_IN ? 1 : 0, 0);
          p = print_fifo_prefix(p, module, group);
          p = print_inst_ids_suffix(p, n, NULL);
        }
      }
    }
  }
  else
  {
    if (!module->to_mem)
    {
      for (int i = 0; i < module->n_io_group; i++)
      {
        struct autosa_array_ref_group *group = module->io_groups[i];
        if (module->in)
        {
          p = print_delimiter(p, &first);
          p = print_fifo_annotation(p, module, group, 1, 0);
          p = print_fifo_prefix(p, module, group);
          p = print_inst_ids_suffix(p, n, NULL);

          if (!boundary)
          {
            p = print_delimiter(p, &first);
            p = print_fifo_annotation(p, module, group, 0, 0);
            p = print_fifo_prefix(p, module, group);
            p = print_inst_ids_inc_suffix(p, n, n - 1, 1);
          }
        }
        else
        {
          if (!boundary)
          {
            p = print_delimiter(p, &first);
            p = print_fifo_annotation(p, module, group, 0, 0);
            p = print_fifo_prefix(p, module, group);
            p = print_inst_ids_inc_suffix(p, n, n - 1, 1);
          }

          p = print_delimiter(p, &first);
          p = print_fifo_annotation(p, module, group, 1, 0);
          p = print_fifo_prefix(p, module, group);
          p = print_inst_ids_suffix(p, n, NULL);
        }
      }
    }
  }

  return p;
}

/* Build the lower-level module name to the current "module".
 */
static char *build_io_module_lower_name(struct autosa_hw_module *module)
{
  struct autosa_array_ref_group *group = module->io_groups[0];

  isl_printer *p = isl_printer_to_str(module->kernel->ctx);
  p = isl_printer_print_str(p, group->array->name);
  if (group->group_type == AUTOSA_IO_GROUP)
  {
    if (group->local_array->n_io_group > 1)
    {
      p = isl_printer_print_str(p, "_");
      p = isl_printer_print_int(p, group->nr);
    }
  }
  else if (group->group_type == AUTOSA_DRAIN_GROUP)
  {
    p = isl_printer_print_str(p, "_");
    p = isl_printer_print_str(p, "drain");
  }
  p = isl_printer_print_str(p, "_IO_L");
  p = isl_printer_print_int(p, module->level - 1);
  if (module->in)
    p = isl_printer_print_str(p, "_in");
  else
    p = isl_printer_print_str(p, "_out");

  char *name = isl_printer_get_str(p);
  isl_printer_free(p);

  return name;
}

/* Print the prefix of fifos to the lower-level modules. 
 */
static __isl_give isl_printer *print_fifo_prefix_lower(
    __isl_take isl_printer *p,
    struct autosa_hw_module *module, struct autosa_array_ref_group *group)
{
  int lower_is_PE;

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"");
  p = autosa_array_ref_group_print_fifo_name(group, p);
  p = isl_printer_print_str(p, "_");
  assert(module->type != PE_MODULE);

  if (module->to_pe)
    lower_is_PE = 1;
  else
    lower_is_PE = 0;

  if (!lower_is_PE)
  {
    char *name = build_io_module_lower_name(module);
    p = isl_printer_print_str(p, name);
    free(name);
  }
  else
  {
    p = isl_printer_print_str(p, "PE");
  }
  p = isl_printer_print_str(p, "\");");
  p = isl_printer_end_line(p);

  return p;
}

/* Print the lower body of the module call, including the 
 * fifos to the lower-level modules.
 */
static __isl_give isl_printer *print_module_call_lower(__isl_take isl_printer *p,
                                                       struct autosa_kernel_stmt *stmt, struct autosa_prog *prog)
{
  struct autosa_hw_module *module = stmt->u.m.module;
  int lower = stmt->u.m.lower;
  int first = 0;
  int n = isl_id_list_n_id(module->inst_ids);
  int lower_is_PE;
  int boundary = stmt->u.m.boundary;

  if (lower)
  {
    struct autosa_array_ref_group *group = module->io_groups[0];

    p = print_delimiter(p, &first);

    p = print_fifo_annotation(p, module, group, module->in ? 0 : 1, 1);
    p = print_fifo_prefix_lower(p, module, group);

    if (module->to_pe)
      lower_is_PE = 1;
    else
      lower_is_PE = 0;

    if (isl_vec_is_zero(group->old_dir) && lower_is_PE && group->pe_io_dir == IO_INOUT)
    {
      /* Add in/out suffix. */
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"");
      p = isl_printer_print_str(p, module->in ? "_in" : "_out");
      p = isl_printer_print_str(p, "\");");
      p = isl_printer_end_line(p);
    }

    if (lower_is_PE)
    {
      p = print_pretrans_inst_ids_suffix(p, module->kernel->n_sa_dim,
                                         boundary ? group->io_pe_expr_boundary : group->io_pe_expr, NULL);
    }
    else
    {
      p = print_inst_ids_suffix(p, n + 1, NULL);
    }
  }

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "p = isl_printer_end_line(p);");
  p = isl_printer_end_line(p);

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "p = isl_printer_indent(p, -4);");
  p = isl_printer_end_line(p);

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "p = isl_printer_start_line(p);");
  p = isl_printer_end_line(p);

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \");\");");
  p = isl_printer_end_line(p);

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "p = isl_printer_end_line(p);");
  p = isl_printer_end_line(p);

  return p;
}

/* Print out the module calls:
 * - module_call_upper
 * - module_call_lower
 */
__isl_give isl_printer *autosa_kernel_print_module_call(
    __isl_take isl_printer *p,
    struct autosa_kernel_stmt *stmt, struct autosa_prog *prog,
    enum platform target)
{
  int upper = stmt->u.m.upper;
  int lower = stmt->u.m.lower;
  int complete = (upper == 0 && lower == 0);
  int dummy = stmt->u.m.dummy;
  int boundary = stmt->u.m.boundary;
  char *module_name = stmt->u.m.module_name;
  struct autosa_hw_module *module = stmt->u.m.module;
  p = ppcg_start_block(p);

  /* Build the module name. */
  if (complete)
  {
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "// Count module number");
    p = isl_printer_end_line(p);

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, module_name);
    if (boundary)
      p = isl_printer_print_str(p, "_boundary");
    p = isl_printer_print_str(p, "_cnt++;");
    p = isl_printer_end_line(p);
    if (module->is_filter && module->is_buffer)
    {
      /* Print counter for inter_trans and intra_trans module. */
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, module_name);
      p = isl_printer_print_str(p, "_intra_trans_cnt++;");
      p = isl_printer_end_line(p);

      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, module_name);
      if (boundary)
        p = isl_printer_print_str(p, "_inter_trans_boundary_cnt++;");
      else
        p = isl_printer_print_str(p, "_inter_trans_cnt++;");
      p = isl_printer_end_line(p);
    }

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_start_line(p);");
    p = isl_printer_end_line(p);

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"/* Module Call */\");");
    p = isl_printer_end_line(p);

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_end_line(p);");
    p = isl_printer_end_line(p);

    p = print_module_call_upper(p, stmt, prog, target);
    p = print_module_call_lower(p, stmt, prog);

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_start_line(p);");
    p = isl_printer_end_line(p);

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"/* Module Call */\");");
    p = isl_printer_end_line(p);

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_end_line(p);");
    p = isl_printer_end_line(p);

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "p = isl_printer_end_line(p);");
    p = isl_printer_end_line(p);
  }
  else
  {
    if (upper)
    {
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "// Count module number");
      p = isl_printer_end_line(p);

      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, module_name);
      if (boundary)
        p = isl_printer_print_str(p, "_boundary");
      p = isl_printer_print_str(p, "_cnt++;");
      p = isl_printer_end_line(p);
      if (module->is_filter && module->is_buffer)
      {
        /* Print counter for inter_trans and intra_trans module */
        p = isl_printer_start_line(p);
        p = isl_printer_print_str(p, module_name);
        p = isl_printer_print_str(p, "_intra_trans_cnt++;");
        p = isl_printer_end_line(p);

        p = isl_printer_start_line(p);
        p = isl_printer_print_str(p, module_name);
        if (boundary)
          p = isl_printer_print_str(p, "_inter_trans_boundary_cnt++;");
        else
          p = isl_printer_print_str(p, "_inter_trans_cnt++;");
        p = isl_printer_end_line(p);
      }

      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "p = isl_printer_start_line(p);");
      p = isl_printer_end_line(p);

      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"/* Module Call */\");");
      p = isl_printer_end_line(p);

      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "p = isl_printer_end_line(p);");
      p = isl_printer_end_line(p);

      p = print_module_call_upper(p, stmt, prog, target);
    }
    else
    {
      p = print_module_call_lower(p, stmt, prog);

      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "p = isl_printer_start_line(p);");
      p = isl_printer_end_line(p);

      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "p = isl_printer_print_str(p, \"/* Module Call */\");");
      p = isl_printer_end_line(p);

      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "p = isl_printer_end_line(p);");
      p = isl_printer_end_line(p);

      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "p = isl_printer_end_line(p);");
      p = isl_printer_end_line(p);
    }
  }

  p = ppcg_end_block(p);

  return p;
}

/* If read, print:
 *   "[fifo_name].read()"
 * else, print:
 *   "[fifo_name].write("
 */
__isl_give isl_printer *print_fifo_rw_xilinx(__isl_take isl_printer *p,
                                             const char *fifo_name, int read)
{
  if (read)
  {
    p = isl_printer_print_str(p, fifo_name);
    p = isl_printer_print_str(p, ".read()");
  }
  else
  {
    p = isl_printer_print_str(p, fifo_name);
    p = isl_printer_print_str(p, ".write(");
  }
  return p;
}

/* If read, print:
 *   "read_channel_intel([fifo_name])"
 * else, print:
 *   "write_channel_intel([fifo_name])"
 */
__isl_give isl_printer *print_fifo_rw_intel(__isl_take isl_printer *p,
                                            const char *fifo_name, int read)
{
  if (read)
  {
    p = isl_printer_print_str(p, "read_channel_intel(");
    p = isl_printer_print_str(p, fifo_name);
    p = isl_printer_print_str(p, ")");
  }
  else
  {
    p = isl_printer_print_str(p, "write_channel_intel(");
    p = isl_printer_print_str(p, fifo_name);
    p = isl_printer_print_str(p, ", ");
  }
  return p;
}

/* Print an I/O statement.
 *
 * An in I/O statement is printed as
 *
 *  local[] = fifo.read(); 
 *
 * while an out I/O statement is printed as
 *
 *  fifo.write(local);
 */
__isl_give isl_printer *autosa_kernel_print_io(__isl_take isl_printer *p,
                                               struct autosa_kernel_stmt *stmt, struct hls_info *hls)
{
  struct autosa_hw_module *module = stmt->u.i.module;
  struct autosa_array_ref_group *group = stmt->u.i.group;
  char *fifo_name;
  isl_ctx *ctx = isl_printer_get_ctx(p);
  int is_dummy = stmt->u.i.dummy;
  fifo_name = concat(ctx, stmt->u.i.fifo_name, stmt->u.i.in == 1 ? "in" : "out");
  int data_pack = stmt->u.i.data_pack;

  if (is_dummy)
  {
    /* [type] fifo_data; */
    p = isl_printer_start_line(p);
    if (data_pack == 1)
    {
      p = isl_printer_print_str(p, group->array->type);
    }
    else
    {
      p = isl_printer_print_str(p, group->array->name);
      p = isl_printer_print_str(p, "_t");
      p = isl_printer_print_int(p, data_pack);
    }
    p = isl_printer_print_str(p, " fifo_data;");
    p = isl_printer_end_line(p);

    /* fifo_data = fifo.read(); */
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "fifo_data = ");
    if (hls->target == XILINX_HW)
      p = print_fifo_rw_xilinx(p, fifo_name, 1);
    else if (hls->target == INTEL_HW)
      p = print_fifo_rw_intel(p, fifo_name, 1);
    p = isl_printer_print_str(p, ";");
    p = isl_printer_end_line(p);

    free(fifo_name);
    return p;
  }

  int nxt_data_pack = stmt->u.i.nxt_data_pack;
  isl_ast_expr *local_index_packed;
  isl_ast_expr *arg, *div;
  int n_arg;
  local_index_packed = isl_ast_expr_copy(stmt->u.i.local_index);
  /* Modify the local index. */
  if (data_pack > 1)
  {
    n_arg = isl_ast_expr_get_op_n_arg(local_index_packed);
    arg = isl_ast_expr_get_op_arg(local_index_packed, n_arg - 1);
    div = isl_ast_expr_from_val(isl_val_int_from_si(ctx, data_pack));
    arg = isl_ast_expr_div(arg, div);
    local_index_packed = isl_ast_expr_set_op_arg(local_index_packed, n_arg - 1, arg);
  }

  if (data_pack == nxt_data_pack)
  {
    /* local[] = fifo.read() */
    p = isl_printer_start_line(p);
    if (stmt->u.i.in)
    {
      p = isl_printer_print_ast_expr(p, local_index_packed);
      p = isl_printer_print_str(p, " = ");
      if (hls->target == XILINX_HW)
        p = print_fifo_rw_xilinx(p, fifo_name, 1);
      else if (hls->target == INTEL_HW)
        p = print_fifo_rw_intel(p, fifo_name, 1);
    }
    else
    {
      /* fifo.write(local[]) */
      if (hls->target == XILINX_HW)
        p = print_fifo_rw_xilinx(p, fifo_name, 0);
      else if (hls->target == INTEL_HW)
        p = print_fifo_rw_intel(p, fifo_name, 0);
      p = isl_printer_print_ast_expr(p, local_index_packed);
      p = isl_printer_print_str(p, ")");
    }
    p = isl_printer_print_str(p, ";");
    p = isl_printer_end_line(p);
  }
  else
  {
    p = ppcg_start_block(p);

    /* [type] fifo_data; */
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, group->array->name);
    p = isl_printer_print_str(p, "_t");
    p = isl_printer_print_int(p, data_pack);
    p = isl_printer_print_str(p, " fifo_data;");
    p = isl_printer_end_line(p);

    if (stmt->u.i.in)
    {
      /* fifo_data = fifo.read(); */
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "fifo_data = ");
      if (hls->target == XILINX_HW)
        p = print_fifo_rw_xilinx(p, fifo_name, 1);
      else if (hls->target == INTEL_HW)
        p = print_fifo_rw_intel(p, fifo_name, 1);
      p = isl_printer_print_str(p, ";");
      p = isl_printer_end_line(p);

      /* for (int n = 0; n < data_pack/nxt_data_pack; n++) { */
      /* #pragma HLS UNROLL */
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "for (int n = 0; n < ");
      p = isl_printer_print_int(p, data_pack / nxt_data_pack);
      p = isl_printer_print_str(p, "; n++) {");
      p = isl_printer_end_line(p);
      if (hls->target == XILINX_HW)
      {
        p = isl_printer_start_line(p);
        p = isl_printer_print_str(p, "#pragma HLS UNROLL");
        p = isl_printer_end_line(p);
      }

      p = isl_printer_indent(p, 4);
      if (hls->target == XILINX_HW)
      {
        isl_ast_expr *op;
        isl_ast_expr *expr = stmt->u.i.local_index;
        int n_arg = isl_ast_expr_op_get_n_arg(expr);
        /* Union */
        if (nxt_data_pack == 1)
        {
          /* union {unsigned int ui; float ut;} u; */
          p = isl_printer_start_line(p);
          p = isl_printer_print_str(p, "union {unsigned int ui; ");
          p = isl_printer_print_str(p, group->array->type);
          p = isl_printer_print_str(p, " ut;} u;");
          p = isl_printer_end_line(p);

          /* u.ui = (unsigned int)fifo_data(32*next_data_pack - 1, 0); */
          p = isl_printer_start_line(p);
          p = isl_printer_print_str(p, "u.ui = (unsigned int)fifo_data(");
          p = isl_printer_print_int(p, group->array->size * 8 * nxt_data_pack - 1);
          p = isl_printer_print_str(p, ", 0);");
          p = isl_printer_end_line(p);
        }

        /* local[][n] = u.ut; or 
         * local[][n] = fifo_data(32*nxt_data_pack - 1, 0);
         */
        p = isl_printer_start_line(p);
        op = isl_ast_expr_op_get_arg(expr, 0);
        p = isl_printer_print_ast_expr(p, op); // array_name
        isl_ast_expr_free(op);
        for (int i = 0; i < n_arg - 1; i++)
        {
          op = isl_ast_expr_op_get_arg(expr, 1 + i);
          p = isl_printer_print_str(p, "[");
          if (i == n_arg - 2)
          {
            p = isl_printer_print_str(p, "n");
          }
          else
          {
            p = isl_printer_print_ast_expr(p, op);
          }
          p = isl_printer_print_str(p, "]");
          isl_ast_expr_free(op);
        }

        p = isl_printer_print_str(p, " = ");
        if (nxt_data_pack == 1)
        {
          p = isl_printer_print_str(p, "u.ut;");
          p = isl_printer_end_line(p);
        }
        else
        {
          p = isl_printer_print_str(p, "fifo_data(");
          p = isl_printer_print_int(p, group->array->size * 8 * nxt_data_pack - 1);
          p = isl_printer_print_str(p, ", 0)");
          p = isl_printer_print_str(p, ";");
          p = isl_printer_end_line(p);
        }

        /* fifo_data = fifo_data >> 32*nxt_data_pack; */
        p = isl_printer_start_line(p);
        p = isl_printer_print_str(p, "fifo_data = fifo_data >> ");
        p = isl_printer_print_int(p, group->array->size * 8 * nxt_data_pack);
        p = isl_printer_print_str(p, ";");
        p = isl_printer_end_line(p);
      }
      else if (hls->target == INTEL_HW)
      {
        // TODO
      }

      p = isl_printer_indent(p, -4);
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "}");
      p = isl_printer_end_line(p);
    }
    else
    {
      if (hls->target == XILINX_HW)
      {
        if (nxt_data_pack == 1)
        {
          /* union {unsigned int ui; float ut;} u1, u0; */
          p = isl_printer_start_line(p);
          p = isl_printer_print_str(p, "union {unsigned int ui; ");
          p = isl_printer_print_str(p, group->array->type);
          p = isl_printer_print_str(p, " ut;} ");
          int first = 1;
          for (int i = data_pack / nxt_data_pack - 1; i >= 0; i--)
          {
            if (!first)
              p = isl_printer_print_str(p, ", ");
            p = isl_printer_print_str(p, "u");
            p = isl_printer_print_int(p, i);
            first = 0;
          }
          p = isl_printer_print_str(p, ";");
          p = isl_printer_end_line(p);

          /* u1 = local[][1];
           * u0 = local[][0];
           */
          for (int i = data_pack / nxt_data_pack - 1; i >= 0; i--)
          {
            isl_ast_expr *expr = stmt->u.i.local_index;
            isl_ast_expr *op;
            int n_arg = isl_ast_expr_op_get_n_arg(expr);

            p = isl_printer_start_line(p);
            p = isl_printer_print_str(p, "u");
            p = isl_printer_print_int(p, i);
            p = isl_printer_print_str(p, ".ut = ");

            op = isl_ast_expr_op_get_arg(expr, 0);
            p = isl_printer_print_ast_expr(p, op);
            isl_ast_expr_free(op);
            for (int j = 0; j < n_arg - 1; j++)
            {
              op = isl_ast_expr_op_get_arg(expr, 1 + j);
              p = isl_printer_print_str(p, "[");
              if (j == n_arg - 2)
              {
                p = isl_printer_print_int(p, i);
              }
              else
              {
                p = isl_printer_print_ast_expr(p, op);
              }
              p = isl_printer_print_str(p, "]");
              isl_ast_expr_free(op);
            }
            p = isl_printer_print_str(p, ";");
            p = isl_printer_end_line(p);
          }
        }

        /* fifo_data = (ap_uint<32*nxt_data_pack>(u1.ui), 
         *              ap_uint<32*nxt_data_pack>(u0.ui)); */
        int first = 1;
        p = isl_printer_start_line(p);
        p = isl_printer_print_str(p, "fifo_data = (");
        for (int i = data_pack / nxt_data_pack - 1; i >= 0; i--)
        {
          isl_ast_expr *expr = stmt->u.i.local_index;
          isl_ast_expr *op;
          int n_arg = isl_ast_expr_op_get_n_arg(expr);

          if (!first)
            p = isl_printer_print_str(p, ", ");

          if (nxt_data_pack == 1)
          {
            p = isl_printer_print_str(p, "ap_uint<");
            p = isl_printer_print_int(p, group->array->size * 8 * nxt_data_pack);
            p = isl_printer_print_str(p, ">(u");
            p = isl_printer_print_int(p, i);
            p = isl_printer_print_str(p, ".ui)");
          }
          else
          {
            op = isl_ast_expr_op_get_arg(expr, 0);
            p = isl_printer_print_ast_expr(p, op);
            isl_ast_expr_free(op);
            for (int j = 0; j < n_arg - 1; j++)
            {
              op = isl_ast_expr_op_get_arg(expr, 1 + j);
              p = isl_printer_print_str(p, "[");
              if (j == n_arg - 2)
              {
                p = isl_printer_print_int(p, i);
              }
              else
              {
                p = isl_printer_print_ast_expr(p, op);
              }
              p = isl_printer_print_str(p, "]");
              isl_ast_expr_free(op);
            }
          }
          first = 0;
        }
        p = isl_printer_print_str(p, ");");
        p = isl_printer_end_line(p);

        p = isl_printer_start_line(p);
        p = print_fifo_rw_xilinx(p, fifo_name, 0);
        p = isl_printer_print_str(p, "fifo_data);");
        p = isl_printer_end_line(p);
      }
      else if (hls->target == INTEL_HW)
      {
        // TODO
      }
    }

    p = ppcg_end_block(p);
  }
  free(fifo_name);
  isl_ast_expr_free(local_index_packed);
  return p;
}

/* Print an I/O transfer statement.
 *
 * An in I/O statement is printed as
 *
 *  [type] fifo_data;
 *  fifo_data = fifo.read();
 *  if (filter_condition) {
 *    local[] = fifo_data; // if buf == 1
 *    fifo_local.write(fifo_data); // if buf == 0
 *  } else {
 *    fifo.write(fifo_data);
 *  }
 *
 * if filter_depth < 0
 *
 *  [type] fifo_data;
 *  fifo_data = fifo.read();
 *  local = fifo_data; // if buf == 1
 *  fifo_local.write(fifo_data); // if buf == 0
 *
 * An out I/O statement is printed as 
 *
 *  [type] fifo_data;
 *  fifo_data = fifo.read();
 *  if (filter_condition) {
 *    fifo_data = local[]; // if buf == 1
 *    fifo_data = fifo_local.read(); // if buf == 0
 *  } else {
 *    fifo_data = fifo.read();
 *  }
 *  fifo.write(fifo_data);
 */
static __isl_give isl_printer *autosa_kernel_print_io_transfer_default(
    __isl_take isl_printer *p, struct autosa_kernel_stmt *stmt,
    struct autosa_array_ref_group *group, int n_lane, struct hls_info *hls)
{
  isl_ctx *ctx;
  char *fifo_name;
  ctx = isl_printer_get_ctx(p);
  int boundary = stmt->u.i.boundary;
  /* If the statement is a boundary statement, 
   * then ignore the filter condition by setting filter_sched_depth as -1
   */
  if (boundary)
    stmt->u.i.filter_sched_depth = -1;

  isl_ast_expr *local_index_packed;
  isl_ast_expr *arg, *div;
  local_index_packed = isl_ast_expr_copy(stmt->u.i.local_index);
  int n_arg;
  /* Modify the local index. */
  if (n_lane > 1)
  {
    n_arg = isl_ast_expr_get_op_n_arg(local_index_packed);
    arg = isl_ast_expr_get_op_arg(local_index_packed, n_arg - 1);
    div = isl_ast_expr_from_val(isl_val_int_from_si(ctx, n_lane));
    arg = isl_ast_expr_div(arg, div);
    local_index_packed = isl_ast_expr_set_op_arg(local_index_packed, n_arg - 1, arg);
  }

  /* Declare the fifo data variable. */
  p = isl_printer_start_line(p);
  if (n_lane == 1)
  {
    p = isl_printer_print_str(p, stmt->u.i.array->type);
  }
  else
  {
    p = isl_printer_print_str(p, stmt->u.i.array->name);
    p = isl_printer_print_str(p, "_t");
    p = isl_printer_print_int(p, n_lane);
  }
  p = isl_printer_print_str(p, " fifo_data;");
  p = isl_printer_end_line(p);

  if (stmt->u.i.in)
  {
    fifo_name = concat(ctx, stmt->u.i.fifo_name, "in");
    /* fifo_data = fifo.read(); */
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "fifo_data");
    p = isl_printer_print_str(p, " = ");
    if (hls->target == XILINX_HW)
      p = print_fifo_rw_xilinx(p, fifo_name, 1);
    else if (hls->target == INTEL_HW)
      p = print_fifo_rw_intel(p, fifo_name, 1);
    p = isl_printer_print_str(p, ";");
    p = isl_printer_end_line(p);
    free(fifo_name);

    /* if (filter_condition) { */
    if (stmt->u.i.filter_sched_depth >= 0)
    {
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "if (c");
      p = isl_printer_print_int(p, stmt->u.i.filter_sched_depth);
      p = isl_printer_print_str(p, " == p");
      p = isl_printer_print_int(p, stmt->u.i.filter_param_id);
      p = isl_printer_print_str(p, ") {");
      p = isl_printer_end_line(p);
      p = isl_printer_indent(p, 2);
    }

    if (stmt->u.i.buf)
    {
      /* local[][] = fifo_data; */
      p = isl_printer_start_line(p);
      p = isl_printer_print_ast_expr(p, local_index_packed);
      p = isl_printer_print_str(p, " = fifo_data;");
      p = isl_printer_end_line(p);
    }
    else
    {
      /* fifo_local.write(fifo_data); */
      fifo_name = concat(ctx, stmt->u.i.fifo_name, "local_out");
      p = isl_printer_start_line(p);
      if (hls->target == XILINX_HW)
        p = print_fifo_rw_xilinx(p, fifo_name, 0);
      else if (hls->target == INTEL_HW)
        p = print_fifo_rw_intel(p, fifo_name, 0);
      p = isl_printer_print_str(p, "fifo_data);");
      p = isl_printer_end_line(p);
      free(fifo_name);
    }

    if (stmt->u.i.filter_sched_depth >= 0)
    {
      p = isl_printer_indent(p, -2);
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "} else {");
      p = isl_printer_end_line(p);
      p = isl_printer_indent(p, 2);

      /* fifo.write(fifo_data); */
      fifo_name = concat(ctx, stmt->u.i.fifo_name, "out");
      p = isl_printer_start_line(p);
      if (hls->target == XILINX_HW)
        p = print_fifo_rw_xilinx(p, fifo_name, 0);
      else if (hls->target == INTEL_HW)
        p = print_fifo_rw_intel(p, fifo_name, 0);
      p = isl_printer_print_str(p, "fifo_data);");
      p = isl_printer_end_line(p);
      free(fifo_name);

      p = isl_printer_indent(p, -2);
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "}");
      p = isl_printer_end_line(p);
    }
  }
  else
  {
    /* if (filter_condition) { */
    if (stmt->u.i.filter_sched_depth >= 0)
    {
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "if (c");
      p = isl_printer_print_int(p, stmt->u.i.filter_sched_depth);
      p = isl_printer_print_str(p, " == p");
      p = isl_printer_print_int(p, stmt->u.i.filter_param_id);
      p = isl_printer_print_str(p, ") {");
      p = isl_printer_end_line(p);
      p = isl_printer_indent(p, 2);
    }

    if (stmt->u.i.buf)
    {
      /* fifo_data = local[][]; */
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "fifo_data = ");
      p = isl_printer_print_ast_expr(p, local_index_packed);
      p = isl_printer_print_str(p, ";");
      p = isl_printer_end_line(p);
    }
    else
    {
      /* fifo_data = fifo_local.read(); */
      fifo_name = concat(ctx, stmt->u.i.fifo_name, "local_in");
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "fifo_data = ");
      if (hls->target == XILINX_HW)
        p = print_fifo_rw_xilinx(p, fifo_name, 1);
      else if (hls->target == INTEL_HW)
        p = print_fifo_rw_intel(p, fifo_name, 1);
      p = isl_printer_print_str(p, ";");
      p = isl_printer_end_line(p);
      free(fifo_name);
    }

    if (stmt->u.i.filter_sched_depth >= 0)
    {
      p = isl_printer_indent(p, -2);
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "} else {");
      p = isl_printer_end_line(p);
      p = isl_printer_indent(p, 2);

      /* fifo_data = fifo.read(); */
      fifo_name = concat(ctx, stmt->u.i.fifo_name, "in");
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "fifo_data = ");
      if (hls->target == XILINX_HW)
        p = print_fifo_rw_xilinx(p, fifo_name, 1);
      else if (hls->target == INTEL_HW)
        p = print_fifo_rw_intel(p, fifo_name, 1);
      p = isl_printer_print_str(p, ";");
      p = isl_printer_end_line(p);
      free(fifo_name);

      p = isl_printer_indent(p, -2);
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "}");
      p = isl_printer_end_line(p);
    }

    /* fifo.write(fifo_data); */
    fifo_name = concat(ctx, stmt->u.i.fifo_name, "out");
    p = isl_printer_start_line(p);
    if (hls->target == XILINX_HW)
      p = print_fifo_rw_xilinx(p, fifo_name, 0);
    else if (hls->target == INTEL_HW)
      p = print_fifo_rw_intel(p, fifo_name, 0);
    p = isl_printer_print_str(p, "fifo_data);");
    p = isl_printer_end_line(p);
    free(fifo_name);
  }

  isl_ast_expr_free(local_index_packed);

  return p;
}

/* Print an I/O transfer statement.
 * is_filter = 0
 * is_buf = 1
 * An in I/O statement is printed as
 *
 *  [type] fifo_data;
 *  [type2] buf_data;
 *  [type] buf_data_split[];
 *  buf_data = local_buf[...];
 *  fifo_data = fifo.read();
 *  for (int n = 0; n < n_lane / nxt_n_lane; n++) {
 *    buf_data_split[n] = buf_data();
 *    buf_data = buf_data >> DW;
 *  }
 *  buf_data_split[...] = Reinterpret<>(fifo_data);
 *  buf_data = (buf_data_split[1], ...);
 *  local_buf[...] = buf_data;
 *
 * An out I/O staement is printed as 
 *
 *  [type] fifo_data;
 *  [type2] buf_data;
 *  [type] buf_data_split[];
 *  buf_data = local_buf[...];
 *  for (int n = 0; n < n_lane / nxt_n_lane; n++) {
 *    buf_data_split[n] = buf_data();
 *    buf_data = buf_data >> DW;
 *  }
 *  fifo_data = Reinterpret<>(buf_data_split[...]);
 *  fifo.write(fifo_data);
 */
static __isl_give isl_printer *autosa_kernel_print_io_transfer_data_pack(
    __isl_take isl_printer *p, struct autosa_kernel_stmt *stmt,
    struct autosa_array_ref_group *group, int n_lane, int nxt_n_lane,
    struct hls_info *hls)
{
  isl_ctx *ctx;
  ctx = isl_printer_get_ctx(p);
  int boundary = stmt->u.i.boundary;

  char *fifo_name;
  isl_ast_expr *expr, *op;
  int n_arg;
  int r;
  isl_val *val;
  isl_ast_expr *local_index_packed;
  isl_ast_expr *arg, *div;
  local_index_packed = isl_ast_expr_copy(stmt->u.i.local_index);
  /* Modify the local index. */
  if (n_lane > 1)
  {
    n_arg = isl_ast_expr_get_op_n_arg(local_index_packed);
    arg = isl_ast_expr_get_op_arg(local_index_packed, n_arg - 1);
    div = isl_ast_expr_from_val(isl_val_int_from_si(ctx, n_lane));
    arg = isl_ast_expr_div(arg, div);
    local_index_packed = isl_ast_expr_set_op_arg(local_index_packed, n_arg - 1, arg);
  }

  /* [type] fifo_data; */
  p = isl_printer_start_line(p);
  if (nxt_n_lane == 1)
  {
    p = isl_printer_print_str(p, group->array->type);
  }
  else
  {
    p = isl_printer_print_str(p, group->array->name);
    p = isl_printer_print_str(p, "_t");
    p = isl_printer_print_int(p, nxt_n_lane);
  }
  p = isl_printer_print_str(p, " ");
  p = isl_printer_print_str(p, "fifo_data;");
  p = isl_printer_end_line(p);

  /* [type2] buf_data; */
  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, group->array->name);
  p = isl_printer_print_str(p, "_t");
  p = isl_printer_print_int(p, n_lane);
  p = isl_printer_print_str(p, " ");
  p = isl_printer_print_str(p, "buf_data;");
  p = isl_printer_end_line(p);

  /* [type] buf_data_split[]; */
  // TODO: move it outside the loop.
  p = isl_printer_start_line(p);
  if (nxt_n_lane == 1)
  {
    p = isl_printer_print_str(p, "ap_uint<");
    p = isl_printer_print_int(p, group->array->size * 8);
    p = isl_printer_print_str(p, ">");
  }
  else
  {
    p = isl_printer_print_str(p, group->array->name);
    p = isl_printer_print_str(p, "_t");
    p = isl_printer_print_int(p, nxt_n_lane);
  }
  p = isl_printer_print_str(p, " buf_data_split[");
  p = isl_printer_print_int(p, n_lane / nxt_n_lane);
  p = isl_printer_print_str(p, "];");
  p = isl_printer_end_line(p);
  if (hls->target == XILINX_HW)
  {
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "#pragma HLS ARRAY_PARTITION variable=buf_data_split complete");
    p = isl_printer_end_line(p);
  }

  if (stmt->u.i.in && stmt->u.i.coalesce_depth >= 0)
  {
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "if (c");
    // TODO: print the iterator index.
    p = isl_printer_print_int(p, stmt->u.i.coalesce_depth);
    p = isl_printer_print_str(p, " % ");
    p = isl_printer_print_int(p, n_lane / nxt_n_lane);
    p = isl_printer_print_str(p, " == 0) {");
    p = isl_printer_end_line(p);
    p = isl_printer_indent(p, 4);
  }
  /* buf_data = local[]; */
  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "buf_data = ");
  p = isl_printer_print_ast_expr(p, local_index_packed);
  p = isl_printer_print_str(p, ";");
  p = isl_printer_end_line(p);

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "for (int n = 0; n < ");
  p = isl_printer_print_int(p, n_lane / nxt_n_lane);
  p = isl_printer_print_str(p, "; n++) {");
  p = isl_printer_end_line(p);

  p = isl_printer_indent(p, 4);
  if (hls->target == XILINX_HW)
  {
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "#pragma HLS UNROLL");
    p = isl_printer_end_line(p);

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "buf_data_split[n] = buf_data(");
    p = isl_printer_print_int(p, group->array->size * 8 * nxt_n_lane - 1);
    p = isl_printer_print_str(p, ", 0);");
    p = isl_printer_end_line(p);

    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "buf_data = buf_data >> ");
    p = isl_printer_print_int(p, group->array->size * 8 * nxt_n_lane);
    p = isl_printer_print_str(p, ";");
    p = isl_printer_end_line(p);
  }

  p = isl_printer_indent(p, -4);
  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "}");
  p = isl_printer_end_line(p);

  if (stmt->u.i.in && stmt->u.i.coalesce_depth >= 0)
  {
    p = isl_printer_indent(p, -4);
    p = print_str_new_line(p, "}");
  }

  /* split_i = ... */
  expr = isl_ast_expr_copy(stmt->u.i.local_index);
  n_arg = isl_ast_expr_op_get_n_arg(expr);
  op = isl_ast_expr_op_get_arg(expr, n_arg - 1);
  r = n_lane / nxt_n_lane;
  val = isl_val_int_from_si(ctx, nxt_n_lane);
  op = isl_ast_expr_div(op, isl_ast_expr_from_val(val));
  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "int split_i = (");
  p = isl_printer_print_ast_expr(p, op);
  p = isl_printer_print_str(p, ") % ");
  p = isl_printer_print_int(p, r);
  p = isl_printer_print_str(p, ";");
  p = isl_printer_end_line(p);
  isl_ast_expr_free(op);
  isl_ast_expr_free(expr);

  if (stmt->u.i.in)
  {
    fifo_name = concat(ctx, stmt->u.i.fifo_name, "in");

    /* fifo_data = fifo.read(); */
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "fifo_data = ");
    if (hls->target == XILINX_HW)
      p = print_fifo_rw_xilinx(p, fifo_name, 1);
    else if (hls->target == INTEL_HW)
      p = print_fifo_rw_intel(p, fifo_name, 1);
    p = isl_printer_print_str(p, ";");
    p = isl_printer_end_line(p);

    if (hls->target == XILINX_HW)
    {
      if (nxt_n_lane == 1)
      {
        p = isl_printer_start_line(p);
        p = isl_printer_print_str(p, "union {unsigned int ui; ");
        p = isl_printer_print_str(p, group->array->type);
        p = isl_printer_print_str(p, " ut;} u;");
        p = isl_printer_end_line(p);

        p = isl_printer_start_line(p);
        p = isl_printer_print_str(p, "u.ut = fifo_data;");
        p = isl_printer_end_line(p);
      }
    }

    /* buf_data_split[...] = Reinterpret<>(fifo_data); */
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "buf_data_split[split_i] = ");
    if (hls->target == XILINX_HW)
    {
      if (nxt_n_lane == 1)
      {
        p = isl_printer_print_str(p, "ap_uint<");
        p = isl_printer_print_int(p, group->array->size * 8);
        p = isl_printer_print_str(p, ">(u.ui);");
      }
      else
      {
        p = isl_printer_print_str(p, "fifo_data;");
      }
    }
    p = isl_printer_end_line(p);

    if (stmt->u.i.coalesce_depth >= 0)
    {
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "if (c");
      p = isl_printer_print_int(p, stmt->u.i.coalesce_depth);
      p = isl_printer_print_str(p, " % ");
      p = isl_printer_print_int(p, n_lane / nxt_n_lane);
      p = isl_printer_print_str(p, " == ");
      p = isl_printer_print_int(p, n_lane / nxt_n_lane);
      p = isl_printer_print_str(p, " - 1 || c");
      p = isl_printer_print_int(p, stmt->u.i.coalesce_depth);
      p = isl_printer_print_str(p, " == ");
      p = isl_printer_print_int(p, stmt->u.i.coalesce_bound - 1);
      p = isl_printer_print_str(p, ") {");
      p = isl_printer_end_line(p);
      p = isl_printer_indent(p, 4);
    }

    /* buf_data = (buf_data_split[1], ...); */
    p = isl_printer_start_line(p);
    if (hls->target == XILINX_HW)
    {
      int first = 1;
      p = isl_printer_print_str(p, "buf_data = (");
      for (int i = n_lane / nxt_n_lane - 1; i >= 0; i--)
      {
        if (!first)
          p = isl_printer_print_str(p, ", ");
        p = isl_printer_print_str(p, "buf_data_split[");
        p = isl_printer_print_int(p, i);
        p = isl_printer_print_str(p, "]");

        first = 0;
      }
      p = isl_printer_print_str(p, ");");
    }
    p = isl_printer_end_line(p);

    /* local_buf[...] = buf_data; */
    p = isl_printer_start_line(p);
    p = isl_printer_print_ast_expr(p, local_index_packed);
    p = isl_printer_print_str(p, " = buf_data;");
    p = isl_printer_end_line(p);

    if (stmt->u.i.coalesce_depth >= 0)
    {
      p = isl_printer_indent(p, -4);
      p = print_str_new_line(p, "}");
    }

    free(fifo_name);
  }
  else
  {
    fifo_name = concat(ctx, stmt->u.i.fifo_name, "out");

    if (hls->target == XILINX_HW)
    {
      if (nxt_n_lane == 1)
      {
        p = isl_printer_start_line(p);
        p = isl_printer_print_str(p, "union {unsigned int ui; ");
        p = isl_printer_print_str(p, group->array->type);
        p = isl_printer_print_str(p, " ut;} u;");
        p = isl_printer_end_line(p);

        p = isl_printer_start_line(p);
        p = isl_printer_print_str(p, "u.ui = (unsigned int)buf_data_split[split_i];");
        p = isl_printer_end_line(p);
      }
    }

    /* fifo_data = Reinterpret<>(buf_data_split[...]); */
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "fifo_data = ");
    if (hls->target == XILINX_HW)
    {
      if (nxt_n_lane == 1)
      {
        p = isl_printer_print_str(p, "u.ut");
      }
      else
      {
        p = isl_printer_print_str(p, "buf_data_split[split_i]");
      }
    }
    p = isl_printer_print_str(p, ";");
    p = isl_printer_end_line(p);

    /* fifo.write(fifo_data); */
    p = isl_printer_start_line(p);
    if (hls->target == XILINX_HW)
      p = print_fifo_rw_xilinx(p, fifo_name, 0);
    else if (hls->target == INTEL_HW)
      p = print_fifo_rw_intel(p, fifo_name, 0);
    p = isl_printer_print_str(p, "fifo_data);");
    p = isl_printer_end_line(p);

    free(fifo_name);
  }

  isl_ast_expr_free(local_index_packed);

  return p;
}

/* Print an I/O transfer statement.
 */
__isl_give isl_printer *autosa_kernel_print_io_transfer(
    __isl_take isl_printer *p,
    struct autosa_kernel_stmt *stmt, struct hls_info *hls)
{
  struct autosa_hw_module *module = stmt->u.i.module;
  struct autosa_array_ref_group *group = stmt->u.i.group;
  int n_lane = stmt->u.i.data_pack;
  int nxt_n_lane = stmt->u.i.nxt_data_pack;
  int is_filter = stmt->u.i.filter;
  int is_buf = stmt->u.i.buf;
  isl_ctx *ctx = isl_printer_get_ctx(p);

  //  p = ppcg_start_block(p);
  if (n_lane == nxt_n_lane)
  {
    p = autosa_kernel_print_io_transfer_default(p, stmt, group, n_lane, hls);
  }
  else
  {
    p = autosa_kernel_print_io_transfer_data_pack(
        p, stmt, group, n_lane, nxt_n_lane, hls);
  }
  //  p = ppcg_end_block(p);

  return p;
}

/* Print an access to the element in the global memory copy
 * described by "stmt".  The index of the copy is recorded in
 * stmt->index as an access to the array.
 */
static __isl_give isl_printer *io_stmt_print_global_index(
    __isl_take isl_printer *p, struct autosa_kernel_stmt *stmt)
{
  struct autosa_array_info *array = stmt->u.i.array;
  isl_ast_expr *index;

  if (autosa_array_is_scalar(array))
  {
    if (!autosa_array_is_read_only_scalar(array))
      p = isl_printer_print_str(p, "*");
    p = isl_printer_print_str(p, array->name);
    return p;
  }

  index = isl_ast_expr_copy(stmt->u.i.index);

  p = isl_printer_print_ast_expr(p, index);
  isl_ast_expr_free(index);

  return p;
}

/* Print an drain merge statement.
 *
 * [group_array_prefix]_to[...] = [group_array_prefix]_from[...]
 */
__isl_give isl_printer *autosa_kernel_print_drain_merge(__isl_take isl_printer *p,
                                                        struct autosa_kernel_stmt *stmt, struct hls_info *hls)
{
  isl_ast_expr *index_to, *index_from, *arg;
  isl_ctx *ctx = hls->ctx;
  struct autosa_drain_merge_func *func = stmt->u.dm.func;
  isl_ast_expr *index = stmt->u.dm.index;
  int n_arg;
  isl_id *id;
  const char *array_name;
  char *new_array_name;
  isl_printer *p_str;

  p = isl_printer_start_line(p);
  // TODO
  n_arg = isl_ast_expr_get_op_n_arg(index);
  /* Modify the index. */
  arg = isl_ast_expr_get_op_arg(index, 0);
  id = isl_ast_expr_id_get_id(arg);
  array_name = isl_id_get_name(id);
  isl_id_free(id);
  isl_ast_expr_free(arg);
  p_str = isl_printer_to_str(ctx);
  p_str = isl_printer_print_str(p_str, array_name);
  p_str = isl_printer_print_str(p_str, "_to");
  new_array_name = isl_printer_get_str(p_str);
  isl_printer_free(p_str);
  id = isl_id_alloc(ctx, new_array_name, NULL);
  arg = isl_ast_expr_from_id(id);
  free(new_array_name);
  index_to = isl_ast_expr_set_op_arg(isl_ast_expr_copy(index), 0, arg);
  //#ifdef _DEBUG
  //  DBGVAR(std::cout, n_arg);
  //  isl_printer *pd = isl_printer_to_file(ctx, stdout);
  //  pd = isl_printer_print_ast_expr(pd, index);
  //  pd = isl_printer_end_line(pd);
  //  pd = isl_printer_print_ast_expr(pd, index_to);
  //  pd = isl_printer_end_line(pd);
  ////  pd = isl_printer_print_ast_expr(pd, arg);
  ////  pd = isl_printer_end_line(pd);
  ////  pd = isl_printer_print_id(pd, id);
  ////  pd = isl_printer_end_line(pd);
  //  pd = isl_printer_free(pd);
  //#endif

  arg = isl_ast_expr_get_op_arg(index, 0);
  id = isl_ast_expr_id_get_id(arg);
  array_name = isl_id_get_name(id);
  isl_id_free(id);
  isl_ast_expr_free(arg);
  p_str = isl_printer_to_str(ctx);
  p_str = isl_printer_print_str(p_str, array_name);
  p_str = isl_printer_print_str(p_str, "_from");
  new_array_name = isl_printer_get_str(p_str);
  isl_printer_free(p_str);
  id = isl_id_alloc(ctx, new_array_name, NULL);
  arg = isl_ast_expr_from_id(id);
  free(new_array_name);
  index_from = isl_ast_expr_set_op_arg(isl_ast_expr_copy(index), 0, arg);

  p = isl_printer_print_ast_expr(p, index_to);
  p = isl_printer_print_str(p, " = ");
  p = isl_printer_print_ast_expr(p, index_from);
  p = isl_printer_print_str(p, ";");

  isl_ast_expr_free(index_to);
  isl_ast_expr_free(index_from);

  p = isl_printer_end_line(p);

  return p;
}

/* Print an I/O dram statement.
 *
 * An in I/O statement is printed as 
 *
 *  [type] fifo_data;
 *  fifo_data = global;
 *  fifo.write(fifo_data);
 *
 * while an out I/O statement is printed as
 *
 *  [type] fifo_data;
 *  fifo_data = fifo.read();
 *  global = fifo_data;
 *
 */
__isl_give isl_printer *autosa_kernel_print_io_dram(__isl_take isl_printer *p,
                                                    struct autosa_kernel_stmt *stmt, struct hls_info *hls)
{
  struct autosa_array_ref_group *group = stmt->u.i.group;
  struct autosa_hw_module *module = stmt->u.i.module;
  char *fifo_name;
  int n_lane = stmt->u.i.data_pack;
  isl_ctx *ctx = isl_printer_get_ctx(p);
  int buf = stmt->u.i.buf;
  isl_ast_expr *local_index_packed;
  local_index_packed = isl_ast_expr_copy(stmt->u.i.local_index);
  int n_arg;
  /* Modify the local index; */
  if (n_lane > 1)
  {
    isl_ast_expr *arg, *div;
    n_arg = isl_ast_expr_get_op_n_arg(local_index_packed);
    arg = isl_ast_expr_get_op_arg(local_index_packed, n_arg - 1);
    div = isl_ast_expr_from_val(isl_val_int_from_si(ctx, n_lane));
    arg = isl_ast_expr_div(arg, div);
    local_index_packed = isl_ast_expr_set_op_arg(local_index_packed, n_arg - 1, arg);
  }

  p = isl_printer_indent(p, -2);
  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "{");
  p = isl_printer_end_line(p);
  p = isl_printer_indent(p, 2);

  /* Declare the fifo data variable. */
  p = isl_printer_start_line(p);
  if (n_lane == 1)
  {
    p = isl_printer_print_str(p, stmt->u.i.array->type);
  }
  else
  {
    p = isl_printer_print_str(p, stmt->u.i.array->name);
    p = isl_printer_print_str(p, "_t");
    p = isl_printer_print_int(p, n_lane);
  }
  p = isl_printer_print_str(p, " fifo_data;");
  p = isl_printer_end_line(p);

  if (stmt->u.i.in)
  {
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "fifo_data = ");
    p = io_stmt_print_global_index(p, stmt);
    p = isl_printer_print_str(p, ";");
    p = isl_printer_end_line(p);

    if (!buf)
    {
      fifo_name = concat(ctx, stmt->u.i.fifo_name, "out");
      p = isl_printer_start_line(p);
      if (hls->target == XILINX_HW)
        p = print_fifo_rw_xilinx(p, fifo_name, 0);
      else if (hls->target == INTEL_HW)
        p = print_fifo_rw_intel(p, fifo_name, 0);
      p = isl_printer_print_str(p, "fifo_data);");
      p = isl_printer_end_line(p);
      free(fifo_name);
    }
    else
    {
      p = isl_printer_start_line(p);
      p = isl_printer_print_ast_expr(p, local_index_packed);
      p = isl_printer_print_str(p, " = fifo_data;");
      p = isl_printer_end_line(p);
    }
  }
  else
  {
    if (!buf)
    {
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "fifo_data = ");
      fifo_name = concat(ctx, stmt->u.i.fifo_name, "in");
      if (hls->target == XILINX_HW)
        p = print_fifo_rw_xilinx(p, fifo_name, 1);
      else if (hls->target == INTEL_HW)
        p = print_fifo_rw_intel(p, fifo_name, 1);
      p = isl_printer_print_str(p, ";");
      p = isl_printer_end_line(p);
      free(fifo_name);
    }
    else
    {
      p = isl_printer_start_line(p);
      p = isl_printer_print_str(p, "fifo_data = ");
      p = isl_printer_print_ast_expr(p, local_index_packed);
      p = isl_printer_print_str(p, ";");
      p = isl_printer_end_line(p);
    }

    p = isl_printer_start_line(p);
    p = io_stmt_print_global_index(p, stmt);
    p = isl_printer_print_str(p, " = fifo_data;");
    p = isl_printer_end_line(p);
  }

  p = isl_printer_indent(p, -2);
  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "}");
  p = isl_printer_end_line(p);
  p = isl_printer_indent(p, 2);

  isl_ast_expr_free(local_index_packed);

  return p;
}

static __isl_give isl_printer *print_inter_trans_module_call(
    __isl_take isl_printer *p,
    struct autosa_hw_module *module, struct autosa_prog *prog,
    struct autosa_kernel *kernel, struct hls_info *hls, int arb, int boundary)
{
  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, module->name);
  p = isl_printer_print_str(p, "_inter_trans");
  if (boundary)
    p = isl_printer_print_str(p, "_boundary");
  p = isl_printer_print_str(p, "(");
  p = print_module_arguments(p, prog, kernel, module, 0,
                             hls->target, 1, arb, boundary);
  p = isl_printer_print_str(p, ");");
  p = isl_printer_end_line(p);

  return p;
}

/* Print the function call for inter_transfer module. */
__isl_give isl_printer *autosa_kernel_print_inter_trans(
    __isl_take isl_printer *p,
    struct autosa_kernel_stmt *stmt, struct hls_info *hls)
{
  struct autosa_hw_module *module = stmt->u.f.module;
  struct autosa_kernel *kernel = module->kernel;
  struct autosa_prog *prog = kernel->prog;
  int boundary = stmt->u.f.boundary;

  if (module->double_buffer)
  {
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "if (arb == 0) {");
    p = isl_printer_end_line(p);
    p = isl_printer_indent(p, 4);
  }

  p = print_inter_trans_module_call(p, module, prog, kernel, hls, 0, boundary);

  if (module->double_buffer)
  {
    p = isl_printer_indent(p, -4);
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "} else {");
    p = isl_printer_end_line(p);
    p = isl_printer_indent(p, 4);

    p = print_inter_trans_module_call(p, module, prog, kernel, hls, 1, boundary);

    p = isl_printer_indent(p, -4);
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "}");
    p = isl_printer_end_line(p);
  }

  return p;
}

static __isl_give isl_printer *print_intra_trans_module_call(
    __isl_take isl_printer *p,
    struct autosa_hw_module *module, struct autosa_prog *prog,
    struct autosa_kernel *kernel,
    struct hls_info *hls, int arb)
{
  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, module->name);
  p = isl_printer_print_str(p, "_intra_trans(");
  p = print_module_arguments(p, prog, kernel, module, 0, hls->target, 0, arb, 0);
  p = isl_printer_print_str(p, ");");
  p = isl_printer_end_line(p);

  return p;
}

/* Print the function call for intra_transfer module. */
__isl_give isl_printer *autosa_kernel_print_intra_trans(
    __isl_take isl_printer *p,
    struct autosa_kernel_stmt *stmt, struct hls_info *hls)
{
  struct autosa_hw_module *module = stmt->u.f.module;
  struct autosa_kernel *kernel = module->kernel;
  struct autosa_prog *prog = kernel->prog;

  if (module->double_buffer)
  {
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "if (arb == 0) {");
    p = isl_printer_end_line(p);
    p = isl_printer_indent(p, 4);
  }

  p = print_intra_trans_module_call(p, module, prog, kernel, hls, 0);

  if (module->double_buffer)
  {
    p = isl_printer_indent(p, -4);
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "} else {");
    p = isl_printer_end_line(p);
    p = isl_printer_indent(p, 4);

    p = print_intra_trans_module_call(p, module, prog, kernel, hls, 1);

    p = isl_printer_indent(p, -4);
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "}");
    p = isl_printer_end_line(p);
  }

  return p;
}

/* Print the function calls for inter_transfer and intra_tranfer modules. */
__isl_give isl_printer *autosa_kernel_print_inter_intra(
    __isl_take isl_printer *p,
    struct autosa_kernel_stmt *stmt, struct hls_info *hls)
{
  struct autosa_hw_module *module = stmt->u.f.module;
  struct autosa_kernel *kernel = module->kernel;
  struct autosa_prog *prog = kernel->prog;
  int boundary = stmt->u.f.boundary;

  if (module->double_buffer)
  {
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "if (arb == 0) {");
    p = isl_printer_end_line(p);
    p = isl_printer_indent(p, 4);
  }

  /* inter_trans */
  p = print_inter_trans_module_call(p, module, prog, kernel, hls, 0, boundary);
  /* intra_trans */
  p = print_intra_trans_module_call(p, module, prog, kernel, hls, 0);

  if (module->double_buffer)
  {
    p = isl_printer_indent(p, -4);
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "} else {");
    p = isl_printer_end_line(p);

    p = isl_printer_indent(p, 4);

    /* inter_trans */
    p = print_inter_trans_module_call(p, module, prog, kernel, hls, 1, boundary);
    /* intra_trans */
    p = print_intra_trans_module_call(p, module, prog, kernel, hls, 1);

    p = isl_printer_indent(p, -4);
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "}");
    p = isl_printer_end_line(p);
  }

  return p;
}

/* Print the function calls for intra_transfer and inter_tranfer modules. */
__isl_give isl_printer *autosa_kernel_print_intra_inter(
    __isl_take isl_printer *p,
    struct autosa_kernel_stmt *stmt, struct hls_info *hls)
{
  struct autosa_hw_module *module = stmt->u.f.module;
  struct autosa_kernel *kernel = module->kernel;
  struct autosa_prog *prog = kernel->prog;
  int boundary = stmt->u.f.boundary;

  if (module->double_buffer)
  {
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "if (arb == 0) {");
    p = isl_printer_end_line(p);
    p = isl_printer_indent(p, 4);
  }

  /* intra_trans */
  p = print_intra_trans_module_call(p, module, prog, kernel, hls, 0);
  /* inter_trans */
  p = print_inter_trans_module_call(p, module, prog, kernel, hls, 0, boundary);

  if (module->double_buffer)
  {
    p = isl_printer_indent(p, -4);
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "} else {");
    p = isl_printer_end_line(p);

    p = isl_printer_indent(p, 4);

    /* intra_trans */
    p = print_intra_trans_module_call(p, module, prog, kernel, hls, 1);
    /* inter_trans */
    p = print_inter_trans_module_call(p, module, prog, kernel, hls, 1, boundary);

    p = isl_printer_indent(p, -4);
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "}");
    p = isl_printer_end_line(p);
  }

  return p;
}

/* Print the state transfer for double buffers. */
__isl_give isl_printer *autosa_kernel_print_state_handle(
    __isl_take isl_printer *p,
    struct autosa_kernel_stmt *stmt, struct hls_info *hls)
{
  struct autosa_hw_module *module = stmt->u.f.module;
  isl_space *space;
  int n;

  if (module->in)
  {
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "intra_trans_en = 1;");
    p = isl_printer_end_line(p);
  }
  else
  {
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, "inter_trans_en = 1;");
    p = isl_printer_end_line(p);
  }

  p = isl_printer_start_line(p);
  p = isl_printer_print_str(p, "arb = !arb;");
  p = isl_printer_end_line(p);

  if (module->in)
  {
    /* intra trans */
    space = module->intra_space;
  }
  else
  {
    /* inter trans */
    space = module->inter_space;
  }
  n = isl_space_dim(space, isl_dim_set);
  for (int i = 0; i < n; i++)
  {
    const char *name;
    name = isl_space_get_dim_name(space, isl_dim_set, i);
    p = isl_printer_start_line(p);
    p = isl_printer_print_str(p, name);
    p = isl_printer_print_str(p, "_prev = ");
    p = isl_printer_print_str(p, name);
    p = isl_printer_print_str(p, ";");
    p = isl_printer_end_line(p);
  }

  return p;
}
