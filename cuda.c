/*
 * Copyright 2010-2011 INRIA Saclay
 *
 * Use of this software is governed by the GNU LGPLv2.1 license
 *
 * Written by Sven Verdoolaege, INRIA Saclay - Ile-de-France,
 * Parc Club Orsay Universite, ZAC des vignes, 4 rue Jacques Monod,
 * 91893 Orsay, France
 */

#include <assert.h>
#include <stdlib.h>

#include <isl/polynomial.h>
#include <isl/union_set.h>
#include <isl/ilp.h>
#include <isl/flow.h>
#include <isl/band.h>
#include <isl/schedule.h>
#include <isl/options.h>
#include <cloog/isl/cloog.h>

#include "cuda.h"
#include "cuda_common.h"
#include "gpucode.h"
#include "schedule.h"
#include "scoplib_isl.h"
#include "ppcg_options.h"

/* The fields stride, shift and shift_map only contain valid information
 * if shift != NULL.
 * If so, they express that current index is such that if you add shift,
 * then the result is always a multiple of stride.
 * shift_map contains the mapping
 *
 *	i -> (i + shift)/stride
 */
struct cuda_array_bound {
	isl_int size;
	isl_qpolynomial *lb;

	isl_int stride;
	isl_qpolynomial *shift;
	isl_basic_map *shift_map;
};

struct cuda_array_info;

/* A group of array references in a kernel that should be handled together.
 * If private_bound is not NULL, then it is mapped to registers.
 * Otherwise, if shared_bound is not NULL, it is mapped to shared memory.
 * Otherwise, it is accesses from global memory.
 */
struct cuda_array_ref_group {
	/* The references in this group access this array. */
	struct cuda_array_info *array;
	/* Position of this group in the list of reference groups of array. */
	int nr;

	/* The following fields are use during the construction of the groups.
	 * access is the combined access relation relative to the shared
	 * memory tiling.
	 * write is set if any access in the group is a write.
	 */
	isl_map *access;
	int write;

	/* For each index, size and offset of piece in shared memory. */
	struct cuda_array_bound *shared_bound;

	/* For each index, size and offset of piece in private memory. */
	struct cuda_array_bound *private_bound;

	/* References in this group; point to elements of a linked list. */
	int n_ref;
	struct cuda_stmt_access **refs;
};

struct cuda_array_info {
	isl_dim *dim;
	/* Name of the array. */
	char *name;
	/* Number of indices. */
	unsigned n_index;
	/* For each index, a bound on the array in that direction. */
	isl_pw_qpolynomial_fold **bound;
	/* For each index, bound[i] specialized to the current kernel. */
	isl_pw_qpolynomial_fold **local_bound;

	/* All references to this array; point to elements of a linked list. */
	int n_ref;
	struct cuda_stmt_access **refs;

	/* The reference groups associated to this array. */
	int n_group;
	struct cuda_array_ref_group **groups;

	/* Last shared memory tile dimension that affects tile of this array. */
	int last_shared;
	/* Dimension at which copying to/from shared memory is printed.
	 * if >= 0, then the value is >= last_shared
	 * if -1, then the copying is done at the leaf level.
	 */
	int print_shared_level;
};

/* Print the name of the local copy of a given group of array references.
 */
static void print_array_name(FILE *out, struct cuda_array_ref_group *group)
{
	int global = 0;

	if (group->private_bound)
		fprintf(out, "private_");
	else if (group->shared_bound)
		fprintf(out, "shared_");
	else
		global = 1;
	fprintf(out, "%s", group->array->name);
	if (!global && group->array->n_group > 1)
		fprintf(out, "_%d", group->nr);
}

/* Collect all references to the given array and store pointers to them
 * in array->refs.
 */
static void collect_references(struct cuda_gen *gen,
	struct cuda_array_info *array)
{
	int i;
	int n;

	n = 0;
	for (i = 0; i < gen->n_stmts; ++i) {
		struct cuda_stmt *stmt = &gen->stmts[i];
		struct cuda_stmt_access *access;

		for (access = stmt->accesses; access; access = access->next) {
			const char *name;
			name = isl_map_get_tuple_name(access->access,
						      isl_dim_out);
			if (name && !strcmp(array->name, name))
				n++;
		}
	}

	array->n_ref = n;
	array->refs = isl_alloc_array(gen->ctx, struct cuda_stmt_access *, n);
	assert(array->refs);

	n = 0;
	for (i = 0; i < gen->n_stmts; ++i) {
		struct cuda_stmt *stmt = &gen->stmts[i];
		struct cuda_stmt_access *access;

		for (access = stmt->accesses; access; access = access->next) {
			const char *name;
			name = isl_map_get_tuple_name(access->access,
						      isl_dim_out);
			if (!name || strcmp(array->name, name))
				continue;

			array->refs[n++] = access;
		}
	}
}

static struct cuda_array_bound *create_bound_list(isl_ctx *ctx, int n_index)
{
	int i;
	struct cuda_array_bound *bound;

	bound = isl_alloc_array(ctx, struct cuda_array_bound, n_index);
	assert(bound);

	for (i = 0; i < n_index; ++i) {
		isl_int_init(bound[i].size);
		bound[i].lb = NULL;
		isl_int_init(bound[i].stride);
		bound[i].shift = NULL;
		bound[i].shift_map = NULL;
	}

	return bound;
}

static void free_bound_list(struct cuda_array_bound *bound, int n_index)
{
	int j;

	if (!bound)
		return;

	for (j = 0; j < n_index; ++j) {
		isl_int_clear(bound[j].size);
		isl_int_clear(bound[j].stride);
		isl_qpolynomial_free(bound[j].lb);
		isl_qpolynomial_free(bound[j].shift);
		isl_basic_map_free(bound[j].shift_map);
	}
	free(bound);
}

/* Compute bounds on the host arrays based on the accessed elements
 * and collect all references to the array.
 */
static int extract_array_info(__isl_take isl_set *array, void *user)
{
	int i;
	struct cuda_gen *gen = (struct cuda_gen *)user;
	const char *name;
	int n_index;
	isl_pw_qpolynomial_fold **bounds;
	isl_pw_qpolynomial_fold **local_bounds;

	n_index = isl_set_dim(array, isl_dim_set);
	name = isl_set_get_tuple_name(array);
	bounds = isl_alloc_array(isl_set_get_ctx(array),
				 isl_pw_qpolynomial_fold *, n_index);
	assert(bounds);
	local_bounds = isl_calloc_array(isl_set_get_ctx(array),
				 isl_pw_qpolynomial_fold *, n_index);
	assert(local_bounds);
	gen->array[gen->n_array].dim = isl_set_get_dim(array);
	gen->array[gen->n_array].name = strdup(name);
	gen->array[gen->n_array].n_index = n_index;
	gen->array[gen->n_array].bound = bounds;
	gen->array[gen->n_array].local_bound = local_bounds;

	for (i = 0; i < n_index; ++i) {
		isl_dim *dim;
		isl_qpolynomial *qp, *one;
		isl_pw_qpolynomial *pwqp;
		isl_pw_qpolynomial_fold *pwf;

		dim = isl_set_get_dim(array);
		one = isl_qpolynomial_one(isl_dim_copy(dim));
		qp = isl_qpolynomial_var(dim, isl_dim_set, i);
		qp = isl_qpolynomial_add(qp, one);
		pwqp = isl_pw_qpolynomial_alloc(isl_set_copy(array), qp);
		pwf = isl_pw_qpolynomial_bound(pwqp, isl_fold_max, NULL);
		pwf = isl_pw_qpolynomial_fold_gist(pwf,
						isl_set_copy(gen->context));

		bounds[i] = pwf;
	}

	collect_references(gen, &gen->array[gen->n_array]);

	gen->n_array++;

	isl_set_free(array);
	return 0;
}

void collect_array_info(struct cuda_gen *gen)
{
	isl_union_set *arrays;

	arrays = isl_union_map_range(isl_union_map_copy(gen->read));
	arrays = isl_union_set_union(arrays,
			isl_union_map_range(isl_union_map_copy(gen->write)));
	arrays = isl_union_set_coalesce(arrays);

	gen->n_array = isl_union_set_n_set(arrays);
	gen->array = isl_alloc_array(gen->ctx,
				     struct cuda_array_info, gen->n_array);
	assert(gen->array);
	gen->n_array = 0;
	isl_union_set_foreach_set(arrays, &extract_array_info, gen);
	isl_union_set_free(arrays);
}

static void free_array_info(struct cuda_gen *gen)
{
	int i, j;

	for (i = 0; i < gen->n_array; ++i) {
		int n_index = gen->array[i].n_index;
		free(gen->array[i].name);
		for (j = 0; j < n_index; ++j) {
			isl_pw_qpolynomial_fold_free(gen->array[i].bound[j]);
			isl_pw_qpolynomial_fold_free(gen->array[i].local_bound[j]);
		}
		isl_dim_free(gen->array[i].dim);
		free(gen->array[i].bound);
		free(gen->array[i].local_bound);
		free(gen->array[i].refs);
	}
	free(gen->array);
}

static void declare_device_arrays(struct cuda_gen *gen)
{
	int i;

	for (i = 0; i < gen->n_array; ++i)
		fprintf(gen->cuda.host_c, "%s *dev_%s;\n",
			gen->options->type, gen->array[i].name);
}

static void print_array_size(struct cuda_gen *gen, FILE *out,
	struct cuda_array_info *array)
{
	int i;
	isl_printer *prn;

	prn = isl_printer_to_file(gen->ctx, out);
	prn = isl_printer_set_output_format(prn, ISL_FORMAT_C);
	for (i = 0; i < array->n_index; ++i) {
		prn = isl_printer_print_str(prn, "(");
		prn = isl_printer_print_pw_qpolynomial_fold(prn,
							    array->bound[i]);
		prn = isl_printer_print_str(prn, ") * ");
	}
	prn = isl_printer_print_str(prn, "sizeof(");
	prn = isl_printer_print_str(prn, gen->options->type);
	prn = isl_printer_print_str(prn, ")");
	isl_printer_free(prn);
}

static void allocate_device_arrays(struct cuda_gen *gen)
{
	int i;

	for (i = 0; i < gen->n_array; ++i) {
		fprintf(gen->cuda.host_c, "cudaMalloc(&dev_%s, ",
			gen->array[i].name);
		print_array_size(gen, gen->cuda.host_c, &gen->array[i]);
		fprintf(gen->cuda.host_c, ");\n");
	}
}

static void free_device_arrays(struct cuda_gen *gen)
{
	int i;

	for (i = 0; i < gen->n_array; ++i)
		fprintf(gen->cuda.host_c, "cudaFree(dev_%s);\n",
			gen->array[i].name);
}

static void copy_arrays_to_device(struct cuda_gen *gen)
{
	int i;

	for (i = 0; i < gen->n_array; ++i) {
		isl_dim *dim;
		isl_set *read_i;
		int empty;

		dim = isl_dim_copy(gen->array[i].dim);
		read_i = isl_union_set_extract_set(gen->copy_in, dim);
		empty = isl_set_fast_is_empty(read_i);
		isl_set_free(read_i);
		if (empty)
			continue;

		fprintf(gen->cuda.host_c, "assert(sizeof(%s) == ",
			gen->array[i].name);
		print_array_size(gen, gen->cuda.host_c, &gen->array[i]);
		fprintf(gen->cuda.host_c, ");\n");
		fprintf(gen->cuda.host_c, "cudaMemcpy(dev_%s, %s, ",
			gen->array[i].name, gen->array[i].name);
		print_array_size(gen, gen->cuda.host_c, &gen->array[i]);
		fprintf(gen->cuda.host_c, ", cudaMemcpyHostToDevice);\n");
	}
}

static void copy_arrays_from_device(struct cuda_gen *gen)
{
	int i;
	isl_union_set *write;
	write = isl_union_map_range(isl_union_map_copy(gen->write));

	for (i = 0; i < gen->n_array; ++i) {
		isl_dim *dim;
		isl_set *write_i;
		int empty;

		dim = isl_dim_copy(gen->array[i].dim);
		write_i = isl_union_set_extract_set(write, dim);
		empty = isl_set_fast_is_empty(write_i);
		isl_set_free(write_i);
		if (empty)
			continue;

		fprintf(gen->cuda.host_c, "cudaMemcpy(%s, dev_%s, ",
			gen->array[i].name, gen->array[i].name);
		print_array_size(gen, gen->cuda.host_c, &gen->array[i]);
		fprintf(gen->cuda.host_c, ", cudaMemcpyDeviceToHost);\n");
	}

	isl_union_set_free(write);
}

static void read_sizes_from_file(struct cuda_gen *gen, const char *filename,
	int *sizes, int len)
{
	int i;
	FILE *file;

	file = fopen(filename, "r");
	if (!file)
		return;

	for (i = 0; i < len; ++i)
		if (fscanf(file, "%d", &sizes[i]) < 1)
			break;

	fclose(file);
}

static void reverse_list(int *list, int len)
{
	int i;
	int t;

	for (i = 0; 2 * i < len; ++i) {
		t = list[i];
		list[i] = list[len - 1 - i];
		list[len - 1 - i] = t;
	}
}

/* Read user specified sizes from "tile.sizes", "block.sizes" and "grid.sizes"
 * after filling in some potentially useful defaults.
 */
static void read_sizes(struct cuda_gen *gen)
{
	int n;

	gen->tile_size = isl_alloc_array(gen->ctx, int, gen->tile_len);
	assert(gen->tile_size);
	for (n = 0; n < gen->tile_len; ++n)
		gen->tile_size[n] = gen->options->tile_size;
	read_sizes_from_file(gen, "tile.sizes", gen->tile_size, gen->tile_len);

	n = gen->n_parallel;
	gen->n_block = (n <= 3) ? n : 3;
	switch (gen->n_block) {
	case 1:
		gen->block_dim[0] = 512;
		break;
	case 2:
		gen->block_dim[0] = 32;
		gen->block_dim[1] = 16;
		break;
	default:
		gen->block_dim[0] = 32;
		gen->block_dim[1] = 4;
		gen->block_dim[2] = 4;
		break;
	}
	read_sizes_from_file(gen, "block.sizes", gen->block_dim, gen->n_block);
	reverse_list(gen->block_dim, gen->n_block);

	gen->n_grid = (n <= 2) ? n : 2;
	switch (gen->n_grid) {
	case 1:
		gen->grid_dim[0] = 65536;
		break;
	default:
		gen->grid_dim[0] = 256;
		gen->grid_dim[1] = 256;
		break;
	}
	read_sizes_from_file(gen, "grid.sizes", gen->grid_dim, gen->n_grid);
	reverse_list(gen->grid_dim, gen->n_grid);
}

static void free_stmts(struct cuda_stmt *stmts, int n)
{
	int i;

	for (i = 0; i < n; ++i) {
		struct cuda_stmt_access *access, *next;

		for (access = stmts[i].accesses; access; access = next) {
			next = access->next;
			isl_map_free(access->access);
			free(access);
		}

		isl_set_free(stmts[i].domain);
		free(stmts[i].text);
	}
	free(stmts);
}

void clear_cuda_gen(struct cuda_gen *gen)
{
	free_stmts(gen->stmts, gen->n_stmts);
	free_array_info(gen);
	isl_set_free(gen->context);
	isl_union_set_free(gen->copy_in);
	isl_union_map_free(gen->sched);
	isl_union_map_free(gen->read);
	isl_union_map_free(gen->write);
}

static void print_reverse_list(FILE *out, int len, int *list)
{
	int i;

	for (i = 0; i < len; ++i) {
		if (i)
			fprintf(out, ", ");
		fprintf(out, "%d", list[len - 1 - i]);
	}
}

static void print_kernel_launch(struct cuda_gen *gen,
	__isl_keep isl_union_set *arrays)
{
	int i;
	int first = 1;
	unsigned nparam;
	isl_dim *dim;

	print_indent(gen->code.dst, gen->code.indent);
	fprintf(gen->code.dst, "kernel%d <<<k%d_dimGrid, k%d_dimBlock>>> (",
		gen->kernel_id, gen->kernel_id, gen->kernel_id);
	fprintf(gen->cuda.kernel_c, "__global__ void kernel%d(",
		gen->kernel_id);
	fprintf(gen->cuda.kernel_h, "__global__ void kernel%d(",
		gen->kernel_id);

	for (i = 0; i < gen->n_array; ++i) {
		isl_dim *dim;
		isl_set *arr;
		int empty;

		dim = isl_dim_copy(gen->array[i].dim);
		arr = isl_union_set_extract_set(arrays, dim);
		empty = isl_set_fast_is_empty(arr);
		isl_set_free(arr);
		if (empty)
			continue;

		if (!first) {
			fprintf(gen->code.dst, ", ");
			fprintf(gen->cuda.kernel_c, ", ");
			fprintf(gen->cuda.kernel_h, ", ");
		}

		fprintf(gen->code.dst, "dev_%s", gen->array[i].name);
		fprintf(gen->cuda.kernel_c, "%s *%s",
			gen->options->type, gen->array[i].name);
		fprintf(gen->cuda.kernel_h, "%s *%s",
			gen->options->type, gen->array[i].name);

		first = 0;
	}

	dim = isl_union_set_get_dim(arrays);
	nparam = isl_dim_size(dim, isl_dim_param);
	for (i = 0; i < nparam; ++i) {
		const char *name = isl_dim_get_name(dim, isl_dim_param, i);
		if (!first) {
			fprintf(gen->code.dst, ", ");
			fprintf(gen->cuda.kernel_c, ", ");
			fprintf(gen->cuda.kernel_h, ", ");
		}
		fprintf(gen->code.dst, "%s", name);
		fprintf(gen->cuda.kernel_c, "int %s", name);
		fprintf(gen->cuda.kernel_h, "int %s", name);
		first = 0;
	}
	isl_dim_free(dim);

	for (i = 0; i < gen->tile_first; ++i) {
		if (!first) {
			fprintf(gen->code.dst, ", ");
			fprintf(gen->cuda.kernel_c, ", ");
			fprintf(gen->cuda.kernel_h, ", ");
		}
		fprintf(gen->code.dst, "h%d", i);
		fprintf(gen->cuda.kernel_c, "int h%d", i);
		fprintf(gen->cuda.kernel_h, "int h%d", i);
		first = 0;
	}

	fprintf(gen->code.dst, ");\n");
	fprintf(gen->cuda.kernel_c, ")\n");
	fprintf(gen->cuda.kernel_h, ");\n");
}

/* Construct a map from a domain of dimensionality "len"
 * to a domain of dimensionality "len" + "tile_len" that tiles
 * the "tile_len" coordinates starting at "first".
 * In particular, [s_i] -> [s_i / tile_size[i], s_i % tile_size[i]].
 * "dim" prescribes the parameters.
 */
static __isl_give isl_map *tile(__isl_take isl_dim *dim, int len,
        int first, int tile_len, int *tile_size)
{
	int i;
	isl_int v;
	isl_basic_map *bmap;
	isl_constraint *c;

	isl_int_init(v);

	dim = isl_dim_add(dim, isl_dim_in, len);
	dim = isl_dim_add(dim, isl_dim_out, len + tile_len);
	bmap = isl_basic_map_universe(isl_dim_copy(dim));

	for (i = 0; i < len - tile_len; ++i) {
		int j = i < first ? i : i + tile_len;
		int k = i < first ? i : i + 2 * tile_len;

		c = isl_equality_alloc(isl_dim_copy(dim));
		isl_int_set_si(v, -1);
		isl_constraint_set_coefficient(c, isl_dim_in, j, v);
		isl_int_set_si(v, 1);
		isl_constraint_set_coefficient(c, isl_dim_out, k, v);
		bmap = isl_basic_map_add_constraint(bmap, c);
	}

	for (i = 0; i < tile_len; ++i) {
		c = isl_equality_alloc(isl_dim_copy(dim));
		isl_int_set_si(v, -1);
		isl_constraint_set_coefficient(c, isl_dim_in, first + i, v);
		isl_int_set_si(v, tile_size[i]);
		isl_constraint_set_coefficient(c, isl_dim_out, first + i, v);
		isl_int_set_si(v, 1);
		isl_constraint_set_coefficient(c, isl_dim_out,
						first + i + tile_len, v);
		bmap = isl_basic_map_add_constraint(bmap, c);

		c = isl_inequality_alloc(isl_dim_copy(dim));
		isl_int_set_si(v, 1);
		isl_constraint_set_coefficient(c, isl_dim_out,
						first + i + tile_len, v);
		bmap = isl_basic_map_add_constraint(bmap, c);
	
		c = isl_inequality_alloc(isl_dim_copy(dim));
		isl_int_set_si(v, -1);
		isl_constraint_set_coefficient(c, isl_dim_out,
						first + i + tile_len, v);
		isl_int_set_si(v, tile_size[i] - 1);
		isl_constraint_set_constant(c, v);
		bmap = isl_basic_map_add_constraint(bmap, c);
	}

	isl_dim_free(dim);
	isl_int_clear(v);

	return isl_map_from_basic_map(bmap);
}

/* Construct a map from a domain of dimensionality "len"
 * to a domain of dimensionality "len" + "wrap_len" that "wraps"
 * the "wrap_len" coordinates starting at "first" according to "wrap_size".
 * In particular, [s_i] -> [s_i, s_i % wrap_size[i]].
 * To do so, we need extra variables corresponding to [s_i / wrap_size[i]],
 * that are projected out at the end.
 * "dim" prescribes the parameters.
 */
static __isl_give isl_map *wrap(__isl_take isl_dim *dim, int len,
        int first, int wrap_len, int *wrap_size)
{
	int i;
	isl_basic_map *bmap;
	isl_constraint *c;

	dim = isl_dim_add(dim, isl_dim_in, len);
	dim = isl_dim_add(dim, isl_dim_out, len + 2 * wrap_len);
	bmap = isl_basic_map_universe(isl_dim_copy(dim));

	for (i = 0; i < len; ++i) {
		int k = i < first + wrap_len ? i : i + 2 * wrap_len;

		c = isl_equality_alloc(isl_dim_copy(dim));
		isl_constraint_set_coefficient_si(c, isl_dim_in, i, -1);
		isl_constraint_set_coefficient_si(c, isl_dim_out, k, 1);
		bmap = isl_basic_map_add_constraint(bmap, c);
	}

	for (i = 0; i < wrap_len; ++i) {
		c = isl_equality_alloc(isl_dim_copy(dim));
		isl_constraint_set_coefficient_si(c, isl_dim_out,
						    first + i, -1);
		isl_constraint_set_coefficient_si(c, isl_dim_out,
						    first + wrap_len + i, 1);
		isl_constraint_set_coefficient_si(c, isl_dim_out,
				    first + 2 * wrap_len + i, wrap_size[i]);
		bmap = isl_basic_map_add_constraint(bmap, c);

		c = isl_inequality_alloc(isl_dim_copy(dim));
		isl_constraint_set_coefficient_si(c, isl_dim_out,
						    first + wrap_len + i, 1);
		bmap = isl_basic_map_add_constraint(bmap, c);
	
		c = isl_inequality_alloc(isl_dim_copy(dim));
		isl_constraint_set_coefficient_si(c, isl_dim_out,
						    first + wrap_len + i, -1);
		isl_constraint_set_constant_si(c, wrap_size[i] - 1);
		bmap = isl_basic_map_add_constraint(bmap, c);
	}

	isl_dim_free(dim);

	bmap = isl_basic_map_project_out(bmap, isl_dim_out,
				first + 2 * wrap_len, wrap_len);

	return isl_map_from_basic_map(bmap);
}

/* Add "n" parameters named prefix%d.
 */
static __isl_give isl_set *add_params( __isl_take isl_set *set,
	int n, const char *prefix)
{
	int i;
	unsigned nparam;
	char name[20];

	nparam = isl_set_dim(set, isl_dim_param);
	set = isl_set_add_dims(set, isl_dim_param, n);

	for (i = 0; i < n; ++i) {
		snprintf(name, sizeof(name), "%s%d", prefix, i);
		set = isl_set_set_dim_name(set, isl_dim_param,
					    nparam + i, name);
	}

	return set;
}

/* Equate the "n" dimensions of "set" starting at "first" to
 * freshly created parameters named prefix%d.
 */
static __isl_give isl_set *parametrize(__isl_take isl_set *set,
	int first, int n, const char *prefix)
{
	int i;
	unsigned nparam;
	isl_int v;
	isl_dim *dim;
	isl_basic_set *bset;
	isl_constraint *c;

	nparam = isl_set_dim(set, isl_dim_param);

	set = add_params(set, n, prefix);

	dim = isl_set_get_dim(set);
	bset = isl_basic_set_universe(isl_dim_copy(dim));

	isl_int_init(v);

	for (i = 0; i < n; ++i) {
		c = isl_equality_alloc(isl_dim_copy(dim));
		isl_int_set_si(v, -1);
		isl_constraint_set_coefficient(c, isl_dim_param, nparam + i, v);
		isl_int_set_si(v, 1);
		isl_constraint_set_coefficient(c, isl_dim_set, first + i, v);
		bset = isl_basic_set_add_constraint(bset, c);
	}

	isl_int_clear(v);
	isl_dim_free(dim);

	return isl_set_intersect(set, isl_set_from_basic_set(bset));
}

static __isl_give isl_set *parametrization(__isl_take isl_dim *dim,
	int len, int first, int n, const char *prefix)
{
	isl_set *set;

	dim = isl_dim_add(dim, isl_dim_set, len);
	set = isl_set_universe(dim);

	return parametrize(set, first, n, prefix);
}

/* Tile the B loops over the tile sizes and then tile/wrap
 * the T1 loops over the blocks.
 */
static __isl_give isl_union_map *tile_schedule(struct cuda_gen *gen,
	__isl_take isl_union_map *sched)
{
	isl_dim *dim;
	isl_map *tiling, *block_tiling;

	dim = isl_union_map_get_dim(sched);
	tiling = tile(isl_dim_copy(dim), gen->untiled_len,
		      gen->tile_first, gen->tile_len, gen->tile_size);

	if (gen->options->wrap)
		block_tiling = wrap(dim, gen->untiled_len + gen->tile_len,
				gen->tile_first, gen->n_grid, gen->grid_dim);
	else
		block_tiling = tile(dim, gen->untiled_len + gen->tile_len,
				gen->tile_first, gen->n_grid, gen->grid_dim);

	gen->tiled_len = gen->untiled_len + gen->tile_len + gen->n_grid;

	tiling = isl_map_apply_range(tiling, block_tiling);

	sched = isl_union_map_apply_range(sched,
					     isl_union_map_from_map(tiling));

	gen->shared_len = gen->tile_first + gen->tile_len + gen->n_grid;

	return sched;
}

static __isl_give isl_union_map *parametrize_tiled_schedule(
	struct cuda_gen *gen, __isl_take isl_union_map *sched)
{
	isl_dim *dim;
	isl_set *par;

	dim = isl_union_map_get_dim(sched);
	par = parametrization(dim, gen->tiled_len, 0, gen->tile_first, "h");
	sched = isl_union_map_intersect_range(sched,
						isl_union_set_from_set(par));

	dim = isl_union_map_get_dim(sched);
	par = parametrization(dim, gen->tiled_len,
		gen->tile_first + gen->n_grid, gen->n_grid, "b");
	sched = isl_union_map_intersect_range(sched,
						isl_union_set_from_set(par));

	return sched;
}

/* Tile/wrap the P1 loops over the threads.
 */
static __isl_give isl_union_map *thread_tile_schedule(struct cuda_gen *gen,
	__isl_take isl_union_map *sched)
{
	isl_dim *dim;
	isl_map *tiling;
	isl_set *par;

	dim = isl_union_map_get_dim(sched);

	if (gen->options->wrap)
		tiling = wrap(isl_dim_copy(dim), gen->tiled_len,
				gen->shared_len, gen->n_block, gen->block_dim);
	else
		tiling = tile(isl_dim_copy(dim), gen->tiled_len,
				gen->shared_len, gen->n_block, gen->block_dim);
	gen->thread_tiled_len = gen->tiled_len + gen->n_block;

	sched = isl_union_map_apply_range(sched,
					     isl_union_map_from_map(tiling));

	par = parametrization(dim, gen->thread_tiled_len,
		gen->tile_first + gen->tile_len + gen->n_grid + gen->n_block,
		gen->n_block, "t");
	sched = isl_union_map_intersect_range(sched,
						isl_union_set_from_set(par));

	gen->shared_len = gen->tile_first + gen->tile_len + gen->n_grid;

	return sched;
}

/* If the user asked for it, scale the shared memory tile loops
 * (T1P and T2) of "sched" by gen->tile_size[i].
 * If we are not performing "wrapping", then additionally scale the T1P
 * loops by gen->grid_dim[i].
 */
static __isl_give isl_union_map *scale_tile_loops(struct cuda_gen *gen,
	__isl_take isl_union_map *sched)
{
	int i;
	isl_dim *dim;
	isl_basic_map *scale;
	isl_constraint *c;

	if (!gen->options->scale_tile_loops)
		return sched;

	dim = isl_union_map_get_dim(sched);
	dim = isl_dim_add(dim, isl_dim_in, gen->tiled_len);
	dim = isl_dim_add(dim, isl_dim_out, gen->tiled_len);
	scale = isl_basic_map_universe(isl_dim_copy(dim));

	for (i = 0; i < gen->tiled_len; ++i) {
		int f = 1;

		if (i >= gen->tile_first && i < gen->tile_first + gen->n_grid) {
			f = gen->tile_size[i - gen->tile_first];
			if (!gen->options->wrap)
				f *= gen->grid_dim[i - gen->tile_first];
		} else if (i >= gen->tile_first + gen->n_grid &&
			   i < gen->tile_first + gen->n_grid + gen->tile_len) {
			f = gen->tile_size[i - (gen->tile_first + gen->n_grid)];
		}

		c = isl_equality_alloc(isl_dim_copy(dim));
		isl_constraint_set_coefficient_si(c, isl_dim_in, i, f);
		isl_constraint_set_coefficient_si(c, isl_dim_out, i, -1);
		scale = isl_basic_map_add_constraint(scale, c);
	}

	isl_dim_free(dim);

	sched = isl_union_map_apply_range(sched,
		isl_union_map_from_map(isl_map_from_basic_map(scale)));

	return sched;
}

/* If we are not performing "wrapping" and if the user asked for it,
 * scale the thread tile loops (P1T) of "sched" by gen->block_dim[i].
 */
static __isl_give isl_union_map *scale_thread_tile_loops(struct cuda_gen *gen,
	__isl_take isl_union_map *sched)
{
	int i;
	isl_dim *dim;
	isl_basic_map *scale;
	isl_constraint *c;

	if (gen->options->wrap)
		return sched;
	if (!gen->options->scale_tile_loops)
		return sched;

	dim = isl_union_map_get_dim(sched);
	dim = isl_dim_add(dim, isl_dim_in, gen->thread_tiled_len);
	dim = isl_dim_add(dim, isl_dim_out, gen->thread_tiled_len);
	scale = isl_basic_map_universe(isl_dim_copy(dim));

	for (i = 0; i < gen->thread_tiled_len; ++i) {
		int f = 1;

		if (i >= gen->shared_len &&
		    i < gen->shared_len + gen->n_block)
			f = gen->block_dim[i - gen->shared_len];

		c = isl_equality_alloc(isl_dim_copy(dim));
		isl_constraint_set_coefficient_si(c, isl_dim_in, i, f);
		isl_constraint_set_coefficient_si(c, isl_dim_out, i, -1);
		scale = isl_basic_map_add_constraint(scale, c);
	}

	isl_dim_free(dim);

	sched = isl_union_map_apply_range(sched,
		isl_union_map_from_map(isl_map_from_basic_map(scale)));

	return sched;
}

/* If we are not performing "wrapping" and if the user asked for it,
 * scale the "n_tile" loops starting at "first" of "sched" by gen->block_dim[i].
 */
static __isl_give isl_union_map *scale_access_tile_loops(struct cuda_gen *gen,
	__isl_take isl_union_map *sched, int len, int first, int n_tile)
{
	int i;
	isl_dim *dim;
	isl_basic_map *scale;
	isl_constraint *c;

	if (gen->options->wrap)
		return sched;
	if (!gen->options->scale_tile_loops)
		return sched;

	dim = isl_union_map_get_dim(sched);
	dim = isl_dim_add(dim, isl_dim_in, len);
	dim = isl_dim_add(dim, isl_dim_out, len);
	scale = isl_basic_map_universe(isl_dim_copy(dim));

	for (i = 0; i < len; ++i) {
		int f = 1;

		if (i >= first && i < first + n_tile)
			f = gen->block_dim[i - first];

		c = isl_equality_alloc(isl_dim_copy(dim));
		isl_constraint_set_coefficient_si(c, isl_dim_in, i, f);
		isl_constraint_set_coefficient_si(c, isl_dim_out, i, -1);
		scale = isl_basic_map_add_constraint(scale, c);
	}

	isl_dim_free(dim);

	sched = isl_union_map_apply_range(sched,
		isl_union_map_from_map(isl_map_from_basic_map(scale)));

	return sched;
}

/* If print_user_stmt is set, we want to print the statements ourselves,
 * instead of relying on the C preprocessor.  If so, we need to use
 * the stop option so that the domains will be saved on the statement
 * nodes.
 */
static void print_cloog_shared_body(struct cuda_gen *gen,
	__isl_keep isl_set *context, __isl_keep isl_union_map *sched, int len,
	void (*print_user_stmt)(struct gpucode_info *info,
				struct clast_user_stmt *s),
	int first_unroll)
{
	int i;
	CloogOptions *options;
	CloogDomain *cloog_context;
	CloogUnionDomain *ud;
	CloogInput *input;
	struct clast_stmt *stmt;
	char name[20];

	sched = isl_union_map_copy(sched);
	sched = isl_union_map_align_params(sched, isl_set_get_dim(context));

	options = cloog_options_malloc(gen->state);
	options->language = LANGUAGE_C;
	options->strides = 1;
	options->sh = 1;
	options->f = len;
	options->l = -1;
	options->override = 1;
	options->save_domains = 1;
	options->noscalars = 1;
	options->first_unroll = first_unroll;

	ud = cloog_union_domain_from_isl_union_map(sched);
	for (i = 0; i < len; ++i) {
		snprintf(name, sizeof(name), "c%d", i);
		ud = cloog_union_domain_set_name(ud, CLOOG_SCAT, i, name);
	}
	cloog_context = cloog_domain_from_isl_set(isl_set_copy(context));
	input = cloog_input_alloc(cloog_context, ud);

	stmt = cloog_clast_create_from_input(input, options);

	gen->stmt_code.indent = gen->kernel_code.indent;
	gen->stmt_code.dst = gen->cuda.kernel_c;
	gen->stmt_code.print_user_stmt = print_user_stmt;
	gen->stmt_code.print_user_stmt_list = NULL;
	gen->stmt_code.print_for_head = NULL;
	gen->stmt_code.print_for_foot = NULL;
	gen->stmt_code.user = gen;
	gpu_print_host_stmt(&gen->stmt_code, stmt);

	cloog_clast_free(stmt);
	cloog_options_free(options);
}

/* Add "len" parameters p[i] called prefix%d,
 * with bounds to 0 <= p[i] < size[i].
 */
__isl_give isl_set *add_bounded_parameters(__isl_take isl_set *set,
	int len, int *size, const char *prefix)
{
	int i;
	unsigned nparam;
	isl_int v;
	isl_dim *dim;
	isl_basic_set *bset;
	isl_constraint *c;
	char name[20];

	nparam = isl_set_dim(set, isl_dim_param);
	set = isl_set_add_dims(set, isl_dim_param, len);

	for (i = 0; i < len; ++i) {
		snprintf(name, sizeof(name), "%s%d", prefix, i);
		set = isl_set_set_dim_name(set, isl_dim_param,
					    nparam + i, name);
	}

	dim = isl_set_get_dim(set);
	bset = isl_basic_set_universe(isl_dim_copy(dim));

	isl_int_init(v);

	for (i = 0; i < len; ++i) {
		c = isl_inequality_alloc(isl_dim_copy(dim));
		isl_int_set_si(v, 1);
		isl_constraint_set_coefficient(c, isl_dim_param, nparam + i, v);
		bset = isl_basic_set_add_constraint(bset, c);
	
		c = isl_inequality_alloc(isl_dim_copy(dim));
		isl_int_set_si(v, -1);
		isl_constraint_set_coefficient(c, isl_dim_param, nparam + i, v);
		isl_int_set_si(v, size[i] - 1);
		isl_constraint_set_constant(c, v);
		bset = isl_basic_set_add_constraint(bset, c);
	}

	isl_int_clear(v);
	isl_dim_free(dim);

	return isl_set_intersect(set, isl_set_from_basic_set(bset));
}

static void print_shared_body(struct cuda_gen *gen,
	__isl_keep isl_set *shared_domain, __isl_keep isl_union_map *sched,
	int len, void (*print_user_stmt)(struct gpucode_info *info,
					 struct clast_user_stmt *s),
	int first_unroll)
{
	isl_set *context;

	context = isl_set_copy(shared_domain);
	context = parametrize(context, 0, gen->shared_len, "g");
	context = isl_set_project_out(context, isl_dim_set, 0, gen->shared_len);
	context = add_bounded_parameters(context,
					gen->n_block, gen->block_dim, "t");

	print_cloog_shared_body(gen, context, sched, len, print_user_stmt,
				first_unroll);

	isl_set_free(context);
}

/* Given a tile of an array, construct a map that maps each element
 * of the tile to a copy of the tile shifted to the origin
 * (based on the lower bounds in group->private_bound or group->shared_bound).
 * If any of the indices is strided, then {private,shared}_bound[i].shift_map
 * is applied to the index first.
 * The domain of the resulting map is "access",
 * while the range space is anonymous.
 */
static __isl_give isl_map *shift_access(__isl_take isl_set *access,
	struct cuda_array_ref_group *group)
{
	int i;
	isl_dim *dim;
	isl_basic_set *bset;
	isl_basic_map *bmap;
	isl_qpolynomial *lb;
	isl_basic_set *offset;
	isl_basic_map *shift;
	isl_basic_map *pre_shift;
	isl_map *sched;
	const char *name;
	struct cuda_array_bound *bounds;
	int n_index = group->array->n_index;

	bounds = group->private_bound;
	if (!bounds)
		bounds = group->shared_bound;

	dim = isl_set_get_dim(access);
	dim = isl_dim_drop(dim, isl_dim_set, 0, n_index);
	offset = isl_basic_set_universe(dim);
	for (i = 0; i < n_index; ++i) {
		lb = isl_qpolynomial_copy(bounds[i].lb);
		bmap = isl_basic_map_from_qpolynomial(lb);
		bset = isl_basic_map_range(bmap);
		offset = isl_basic_set_flat_product(offset, bset);
	}
	offset = isl_basic_set_neg(offset);

	dim = isl_dim_map_from_set(isl_set_get_dim(access));
	shift = isl_basic_map_identity(dim);
	shift = isl_basic_map_set_tuple_name(shift, isl_dim_out, NULL);

	bset = isl_basic_set_universe(isl_set_get_dim(access));
	bmap = isl_basic_map_from_domain_and_range(bset, offset);

	shift = isl_basic_map_sum(shift, bmap);

	dim = isl_set_get_dim(access);
	dim = isl_dim_drop(dim, isl_dim_set, 0, n_index);
	dim = isl_dim_map_from_set(dim);
	pre_shift = isl_basic_map_universe(isl_dim_copy(dim));
	dim = isl_dim_add(dim, isl_dim_in, 1);
	dim = isl_dim_add(dim, isl_dim_out, 1);
	for (i = 0; i < n_index; ++i) {
		if (!bounds[i].shift_map)
			bmap = isl_basic_map_identity(isl_dim_copy(dim));
		else
			bmap = isl_basic_map_copy(bounds[i].shift_map);
		pre_shift = isl_basic_map_flat_product(pre_shift, bmap);
	}
	isl_dim_free(dim);
	name = isl_basic_map_get_tuple_name(shift, isl_dim_in);
	pre_shift = isl_basic_map_set_tuple_name(pre_shift, isl_dim_in, name);
	pre_shift = isl_basic_map_set_tuple_name(pre_shift, isl_dim_out, name);
	shift = isl_basic_map_apply_range(pre_shift, shift);

	sched = isl_map_from_basic_map(shift);
	sched = isl_map_intersect_domain(sched, access);

	return sched;
}

/* Construct a schedule for iterating over all elements in the given
 * piece of an array.  The schedule iterates over a copy of the piece
 * that is shifted to the origin.
 * We subsequently also perform the tiling/wrapping over the threads.
 *
 * In particular, we tile the final iterators so that the final thread
 * dimension runs over the final array dimension.
 * However, if those final iterators have only a single iteration,
 * we try to tile earlier iterators instead.
 */
static __isl_give isl_union_map *access_schedule(struct cuda_gen *gen,
	__isl_take isl_set *access, struct cuda_array_ref_group *group)
{
	isl_dim *dim;
	isl_map *sched;
	isl_union_map *usched;
	isl_map *tiling;
	isl_set *par;
	unsigned nvar = isl_set_dim(access, isl_dim_set);
	int n_tile;
	int first;

	sched = shift_access(access, group);

	n_tile = gen->n_block;
	if (n_tile > nvar) {
		int i;
		sched = isl_map_insert(sched, isl_dim_out, 0, n_tile - nvar);
		for (i = 0; i < n_tile - nvar; ++i)
			sched = isl_map_fix_si(sched, isl_dim_out, i, 0);
		nvar = n_tile;
	}

	first = nvar - n_tile;

	for (; first > 0; first --)
		if (!isl_map_plain_is_fixed(sched, isl_dim_out,
						first + n_tile - 1, NULL))
			break;

	dim = isl_map_get_dim(sched);
	dim = isl_dim_drop(dim, isl_dim_in, 0, isl_dim_size(dim, isl_dim_in));
	dim = isl_dim_drop(dim, isl_dim_out, 0, nvar);
	if (gen->options->wrap)
		tiling = wrap(isl_dim_copy(dim), nvar, first,
				n_tile, gen->block_dim);
	else
		tiling = tile(isl_dim_copy(dim), nvar, first,
				n_tile, gen->block_dim);
	sched = isl_map_apply_range(sched, tiling);

	par = parametrization(dim, nvar + n_tile, first + n_tile, n_tile, "t");
	usched = isl_union_map_from_map(sched);
	usched = isl_union_map_intersect_range(usched,
						isl_union_set_from_set(par));

	usched = scale_access_tile_loops(gen, usched, nvar + n_tile,
					 first, n_tile);

	return usched;
}

static void print_shared_access(struct cuda_gen *gen,
	__isl_keep isl_set *shared_domain, __isl_take isl_set *access,
	const char *type, struct cuda_array_ref_group *group)
{
	const char *array_name;
	char *name;
	isl_ctx *ctx;
	isl_union_map *sched;
	unsigned nvar = isl_set_dim(access, isl_dim_set);
	int n_tile;

	ctx = isl_set_get_ctx(access);
	array_name = isl_set_get_tuple_name(access);
	name = isl_alloc_array(ctx, char,
		strlen(type) + sizeof("_shared_") + strlen(array_name) + 20);
	if (group->array->n_group > 1)
		sprintf(name, "%s_shared_%s_%d", type, array_name, group->nr);
	else
		sprintf(name, "%s_shared_%s", type, array_name);
	access = isl_set_set_tuple_name(access, name);
	free(name);

	sched = access_schedule(gen, access, group);

	n_tile = gen->n_block;
	if (n_tile > nvar)
		n_tile = nvar;

	print_shared_body(gen, shared_domain, sched, nvar + n_tile, NULL, -1);

	isl_union_map_free(sched);
}

/* Return the union of all read (read = 1) and/or write (write = 1)
 * access relations in the group.
 */
static __isl_give isl_union_map *group_access_relation(
	struct cuda_array_ref_group *group, int read, int write)
{
	int i;
	isl_union_map *access;

	access = isl_union_map_empty(isl_map_get_dim(group->access));
	for (i = 0; i < group->n_ref; ++i) {
		isl_map *map_i;

		if (!((read && group->refs[i]->read) ||
		     (write && group->refs[i]->write)))
			continue;
		map_i = isl_map_copy(group->refs[i]->access);
		access = isl_union_map_union(access,
					    isl_union_map_from_map(map_i));
	}

	return access;
}

/* Print code for reading into or writing from shared memory
 * the given array reference group.
 *
 * sched maps the original iteration domains to the shared memory tile loops.
 */
static int print_group_shared_accesses(struct cuda_gen *gen,
	struct cuda_array_ref_group *group, const char *type,
	__isl_keep isl_set *shared_domain, __isl_keep isl_union_map *sched)
{
	int read;
	isl_union_map *access;
	isl_union_set *uset;
	isl_set *access_set;

	if (group->private_bound)
		return 0;
	if (!group->shared_bound)
		return 0;

	read = !strcmp(type, "read");

	access = group_access_relation(group, read, !read);
	access = isl_union_map_apply_domain(access, isl_union_map_copy(sched));
	uset = isl_union_map_range(access);

	if (isl_union_set_is_empty(uset)) {
		isl_union_set_free(uset);
		return 0;
	}

	access_set = isl_union_set_copy_set(uset);
	isl_union_set_free(uset);
	access_set = isl_set_coalesce(access_set);

	print_shared_access(gen, shared_domain, access_set, type, group);

	return 1;
}

/* Print code for reading into or writing from shared memory at
 * the given level (-1 for innermost).
 *
 * If we are not printing at the innermost level, then the dimensionality
 * of shared_domain may be smaller than gen->shared_len.
 * As the rest of the code assumes that the domain of access has
 * gen->shared_len dimensions, we therefore may need to embed this domain
 * in a higher dimensional space after intersection with shared_domain.
 */
static void print_shared_accesses(struct cuda_gen *gen,
	__isl_keep isl_set *shared_domain, __isl_keep isl_union_map *access,
	const char *type, int level)
{
	int i, j;
	isl_dim *dim;
	isl_map *proj;
	isl_set *par;
	int shared_len = isl_set_dim(shared_domain, isl_dim_set);
	int sync = 0;
	isl_union_map *sched;

	shared_domain = isl_set_copy(shared_domain);
	sched = isl_union_map_copy(gen->tiled_sched);
	dim = isl_union_map_get_dim(sched);
	proj = projection(dim, gen->tiled_len, shared_len);
	sched = isl_union_map_apply_range(sched, isl_union_map_from_map(proj));
	sched = isl_union_map_intersect_range(sched,
			isl_union_set_from_set(isl_set_copy(shared_domain)));
	if (shared_len != gen->shared_len) {
		dim = isl_union_map_get_dim(sched);
		proj = projection(dim, gen->shared_len, shared_len);
		proj = isl_map_reverse(proj);
		shared_domain = isl_set_apply(shared_domain,
						isl_map_copy(proj));
		sched = isl_union_map_apply_range(sched,
				isl_union_map_from_map(proj));
	}

	dim = isl_union_map_get_dim(sched);
	par = parametrization(dim, gen->shared_len, 0, gen->shared_len, "g");
	sched = isl_union_map_intersect_range(sched,
						isl_union_set_from_set(par));

	for (i = 0; i < gen->n_array; ++i) {
		struct cuda_array_info *array = &gen->array[i];

		if (gen->array[i].print_shared_level != level)
			continue;

		for (j = 0; j < array->n_group; ++j) {
			if (print_group_shared_accesses(gen, array->groups[j],
					    type, shared_domain, sched))
				sync = 1;
		}
	}

	isl_union_map_free(sched);
	isl_set_free(shared_domain);

	if (sync) {
		print_indent(gen->cuda.kernel_c, gen->kernel_code.indent);
		fprintf(gen->cuda.kernel_c, "__syncthreads();\n");
	}
}

/* Given an index expression into a tile of an array, adjust the expression
 * to a shift of the tile to the origin
 * (based on the lower bounds in array->shared_bound).
 * If the index is strided, then we first add
 * bound->shift and divide by bound->stride.
 */
static __isl_give isl_qpolynomial *shift_index(__isl_take isl_qpolynomial *qp,
	struct cuda_array_info *array,
	struct cuda_array_bound *bound, __isl_take isl_set *domain)
{
	isl_qpolynomial *lb;

	if (bound->shift) {
		isl_qpolynomial *shift, *t;
		isl_int one;
		isl_dim *dim;
		shift = bound->shift;
		shift = isl_qpolynomial_copy(shift);
		shift = isl_qpolynomial_drop_dims(shift, isl_dim_set, 0,
			    isl_qpolynomial_dim(shift, isl_dim_set));
		shift = isl_qpolynomial_align_params(shift,
					  isl_qpolynomial_get_dim(qp));
		qp = isl_qpolynomial_add(qp, shift);
		dim = isl_qpolynomial_get_dim(qp);
		isl_int_init(one);
		isl_int_set_si(one, 1);
		t = isl_qpolynomial_rat_cst(dim, one, bound->stride);
		isl_int_clear(one);
		qp = isl_qpolynomial_mul(qp, t);
	}

	lb = isl_qpolynomial_copy(bound->lb);
	lb = isl_qpolynomial_drop_dims(lb, isl_dim_set, 0,
			isl_qpolynomial_dim(lb, isl_dim_set));

	lb = isl_qpolynomial_align_params(lb, isl_qpolynomial_get_dim(qp));

	qp = isl_qpolynomial_sub(qp, lb);
	qp = isl_qpolynomial_gist(qp, domain);

	return qp;
}

/* This function is called for each access to an array in some statement
 * in the original code.
 * Replace that access by an access to shared or (linearized) global memory.
 * Since the array in shared memory is just
 * a shifted copy of part of the original array, we simply need
 * to subtract the lower bound, which was computed
 * in can_tile_for_shared_memory.
 * If any of the indices is strided, then we first add
 * shared_bound[i].shift and divide by shared_bound[i].stride.
 *
 * If the given array is accessed directly from global memory,
 * we don't need to perform any shifting and simply simplify
 * expression in the context of the domain instead.
 *
 * If the array space (range of access) has no name, then we are
 * accessing an iterator in the original program.
 */
static void print_access(struct cuda_gen *gen, __isl_take isl_map *access,
	int group_nr)
{
	int i;
	const char *name;
	unsigned n_index;
	struct cuda_array_info *array = NULL;
	isl_printer *prn;
	isl_basic_set *aff;
	isl_set *data_set;
	isl_set *domain;
	struct cuda_array_bound *bounds = NULL;

	access = isl_map_align_params(access,
					isl_set_get_dim(gen->stmt_domain));

	data_set = isl_set_apply(isl_set_copy(gen->stmt_domain), access);

	name = isl_set_get_tuple_name(data_set);

	if (!name)
		fprintf(gen->cuda.kernel_c, "(");
	else {
		struct cuda_array_ref_group *group;

		for (i = 0; i < gen->n_array; ++i) {
			if (strcmp(name, gen->array[i].name))
				continue;
			array = &gen->array[i];
		}
		assert(array);
		group = array->groups[group_nr];
		bounds = group->private_bound;
		if (!bounds)
			bounds = group->shared_bound;

		print_array_name(gen->cuda.kernel_c, group);
		fprintf(gen->cuda.kernel_c, "[");
	}


	n_index = isl_set_dim(data_set, isl_dim_set);
	aff = isl_set_affine_hull(data_set);

	prn = isl_printer_to_file(gen->ctx, gen->cuda.kernel_c);
	prn = isl_printer_set_output_format(prn, ISL_FORMAT_C);

	if (!bounds)
		for (i = 0; i + 1 < n_index; ++i)
			prn = isl_printer_print_str(prn, "(");

	for (i = 0; i < n_index; ++i) {
		isl_constraint *c;
		isl_qpolynomial *qp;
		int ok;

		ok = isl_basic_set_has_defining_equality(aff,
							isl_dim_out, i, &c);
		assert(ok);
		qp = isl_qpolynomial_from_constraint(c, isl_dim_out, i);
		qp = isl_qpolynomial_drop_dims(qp, isl_dim_set, 0,
				isl_qpolynomial_dim(qp, isl_dim_set));

		if (!array) {
			prn = isl_printer_print_qpolynomial(prn, qp);
			isl_qpolynomial_free(qp);
			continue;
		}

		domain = isl_set_copy(gen->stmt_domain);
		domain = isl_set_project_out(domain, isl_dim_set, 0,
					    isl_set_dim(domain, isl_dim_set));
		if (!bounds)
			qp = isl_qpolynomial_gist(qp, domain);
		else
			qp = shift_index(qp, array, &bounds[i], domain);

		if (i) {
			if (!bounds) {
				prn = isl_printer_print_str(prn, ") * (");
				prn = isl_printer_print_pw_qpolynomial_fold(prn,
							array->local_bound[i]);
				prn = isl_printer_print_str(prn, ") + ");
			} else
				prn = isl_printer_print_str(prn, "][");
		}
		prn = isl_printer_print_qpolynomial(prn, qp);
		isl_qpolynomial_free(qp);
	}
	if (!name)
		prn = isl_printer_print_str(prn, ")");
	else
		prn = isl_printer_print_str(prn, "]");
	isl_printer_free(prn);

	isl_basic_set_free(aff);
}

static void print_stmt_body(struct cuda_gen *gen,
	FILE *out, struct cuda_stmt *stmt)
{
	int last = 0;
	struct cuda_stmt_access *access;

	for (access = stmt->accesses; access; access = access->next) {
		fwrite(stmt->text + last, 1, access->text_offset - last, out);
		last = access->text_offset + access->text_len;

		print_access(gen, isl_map_copy(access->access),
			     access->group);
	}

	fprintf(out, "%s\n", stmt->text + last);
}

/* This function is called for each leaf in the innermost clast,
 * i.e., for each statemetn.
 * We print the statement body, simplifying the accesses based
 * on the schedule.
 */
static void print_statement(struct gpucode_info *code,
	struct clast_user_stmt *u)
{
	struct cuda_gen *gen = code->user;
	isl_dim *dim;
	isl_set *par;
	isl_set *stmt_domain;
	isl_union_map *stmt_sched;
	isl_union_set *uset;
	int nr;
	struct cuda_stmt *stmt;

	nr = atoi(u->statement->name + 2);
	stmt = &gen->stmts[nr];

	stmt_domain = extract_host_domain(u);

	stmt_sched = isl_union_map_intersect_range(
		    isl_union_map_copy(gen->local_sched),
		    isl_union_set_from_set(extend(stmt_domain,
						  gen->thread_tiled_len)));
	dim = isl_union_map_get_dim(stmt_sched);
	par = parametrization(dim, gen->thread_tiled_len, 0,
				gen->thread_tiled_len, "c");
	stmt_sched = isl_union_map_intersect_range(stmt_sched,
						isl_union_set_from_set(par));

	uset = isl_union_map_domain(stmt_sched);
	dim = isl_union_set_get_dim(uset);
	dim = isl_dim_add(dim, isl_dim_set,
			  isl_set_dim(stmt->domain, isl_dim_set));
	dim = isl_dim_set_tuple_name(dim, isl_dim_set, u->statement->name);
	gen->stmt_domain = isl_union_set_extract_set(uset, dim);
	isl_union_set_free(uset);

	print_indent(code->dst, code->indent);
	print_stmt_body(gen, code->dst, stmt);

	isl_set_free(gen->stmt_domain);
}

/* Print an access to the element in the global memory copy of the
 * given array that corresponds to element [qp[0]][qp[1]]...
 * of the original array.
 * The copy in global memory has been linearized, so we need to take
 * the array size into account.
 */
static void print_private_global_index(isl_ctx *ctx, FILE *out,
	struct cuda_array_info *array, __isl_keep isl_qpolynomial **qp)
{
	int i;
	isl_printer *prn;

	fprintf(out, "%s[", array->name);
	prn = isl_printer_to_file(ctx, out);
	prn = isl_printer_set_output_format(prn, ISL_FORMAT_C);
	for (i = 0; i + 1 < array->n_index; ++i)
		prn = isl_printer_print_str(prn, "(");
	for (i = 0; i < array->n_index; ++i) {
		if (i) {
			prn = isl_printer_print_str(prn, ") * (");
			prn = isl_printer_print_pw_qpolynomial_fold(prn,
							array->local_bound[i]);
			prn = isl_printer_print_str(prn, ") + ");
		}
		prn = isl_printer_print_qpolynomial(prn, qp[i]);
	}
	isl_printer_free(prn);
	fprintf(out, "]");
}

/* Print an access to the element in the shared memory copy of the
 * given array reference group that corresponds to element [qps[0]][qps[1]]...
 * of the original array.
 * Since the array in shared memory is just a shifted copy of part
 * of the original array, we simply need to subtract the lower bound,
 * which was computed in can_tile_for_shared_memory.
 * If any of the indices is strided, then we first add
 * shared_bound[i].shift and divide by shared_bound[i].stride.
 */
static void print_private_local_index(isl_ctx *ctx, FILE *out,
	struct cuda_array_ref_group *group,
	__isl_keep isl_qpolynomial **qps, __isl_keep isl_set *domain)
{
	int i;
	isl_printer *prn;
	struct cuda_array_info *array = group->array;
	struct cuda_array_bound *bounds = group->private_bound;

	print_array_name(out, group);
	for (i = 0; i < array->n_index; ++i) {
		isl_qpolynomial *qp = isl_qpolynomial_copy(qps[i]);

		qp = shift_index(qp, array, &bounds[i], isl_set_copy(domain));

		fprintf(out, "[");
		prn = isl_printer_to_file(ctx, out);
		prn = isl_printer_set_output_format(prn, ISL_FORMAT_C);
		prn = isl_printer_print_qpolynomial(prn, qp);
		isl_printer_free(prn);
		fprintf(out, "]");
		isl_qpolynomial_free(qp);
	}
}

/* This function is called for each leaf in the clast of the code
 * for copying to or from private memory.
 * The statement name is read_private_<array> or write_private_<array>.
 *
 * The schedule iterates over the array elements, so we can use
 * the domain of private_sched at the current scheduling position
 * as the index of the array.
 */
static void print_private_copy_statement(struct gpucode_info *code,
	struct clast_user_stmt *u)
{
	struct cuda_gen *gen = code->user;
	isl_set *domain;
	isl_map *sched;
	struct cuda_array_ref_group *group = gen->private_group;
	int i;
	unsigned n_in;
	unsigned n_out;
	isl_dim *dim;
	isl_set *param;
	isl_set *index;
	isl_basic_set *aff;
	isl_ctx *ctx;
	isl_qpolynomial **qp;
	int read;

	read = !strncmp(u->statement->name, "read", 4);

	domain = extract_host_domain(u);
	assert(domain);

	sched = isl_map_copy(gen->private_sched);
	sched = isl_map_reverse(sched);
	sched = isl_map_intersect_domain(sched, domain);
	n_in = isl_map_dim(sched, isl_dim_in);
	n_out = isl_map_dim(sched, isl_dim_out);
	dim = isl_map_get_dim(sched);
	dim = isl_dim_drop(dim, isl_dim_in, 0, n_in);
	dim = isl_dim_drop(dim, isl_dim_out, 0, n_out);
	param = parametrization(dim, n_in, 0, n_in, "c");
	sched = isl_map_align_params(sched, isl_set_get_dim(param));
	sched = isl_map_intersect_domain(sched, param);
	index = isl_map_range(sched);
	domain = isl_set_copy(index);
	aff = isl_set_affine_hull(index);
	domain = isl_set_project_out(domain, isl_dim_set, 0, n_out);

	ctx = isl_basic_set_get_ctx(aff);
	qp = isl_alloc_array(ctx, isl_qpolynomial *, n_out);
	assert(qp);

	for (i = 0; i < n_out; ++i) {
		isl_constraint *c;
		int ok;

		ok = isl_basic_set_has_defining_equality(aff,
							isl_dim_set, i, &c);
		assert(ok);
		qp[i] = isl_qpolynomial_from_constraint(c, isl_dim_set, i);
		qp[i] = isl_qpolynomial_drop_dims(qp[i], isl_dim_set, 0, n_out);
	}

	print_indent(code->dst, code->indent);
	if (read) {
		print_private_local_index(ctx, code->dst, group, qp, domain);
		fprintf(code->dst, " = ");
		print_private_global_index(ctx, code->dst, group->array, qp);
	} else {
		print_private_global_index(ctx, code->dst, group->array, qp);
		fprintf(code->dst, " = ");
		print_private_local_index(ctx, code->dst, group, qp, domain);
	}
	fprintf(code->dst, ";\n");

	for (i = 0; i < n_out; ++i)
		isl_qpolynomial_free(qp[i]);
	free(qp);

	isl_basic_set_free(aff);
	isl_set_free(domain);
}

static void print_private_access(struct cuda_gen *gen,
	__isl_keep isl_set *shared_domain, __isl_take isl_set *access,
	const char *type, struct cuda_array_ref_group *group)
{
	const char *array_name;
	char *name;
	isl_ctx *ctx;
	unsigned nvar = isl_set_dim(access, isl_dim_set);
	isl_union_map *usched;

	if (isl_set_fast_is_empty(access)) {
		isl_set_free(access);
		return;
	}

	ctx = isl_set_get_ctx(access);
	array_name = isl_set_get_tuple_name(access);
	name = isl_alloc_array(ctx, char,
		strlen(type) + sizeof("_private_") + strlen(array_name) + 20);
	if (group->array->n_group > 1)
		sprintf(name, "%s_private_%s_%d", type, array_name, group->nr);
	else
		sprintf(name, "%s_private_%s", type, array_name);
	access = isl_set_set_tuple_name(access, name);
	free(name);

	gen->private_sched = shift_access(access, group);
	gen->private_group = group;

	usched = isl_union_map_from_map(isl_map_copy(gen->private_sched));
	print_shared_body(gen, shared_domain, usched, nvar,
				&print_private_copy_statement, 1);
	isl_union_map_free(usched);

	isl_map_free(gen->private_sched);
}

/* Print code for reading into or writing from private memory
 * the given array reference group.
 *
 * sched maps the original iteration domains to the shared memory tile loops.
 */
static void print_group_private_accesses(struct cuda_gen *gen,
	struct cuda_array_ref_group *group,
	 const char *type, __isl_keep isl_set *shared_domain,
	unsigned first_shared, int shared_len, __isl_keep isl_union_map *sched)
{
	int read;
	isl_union_map *access;
	isl_union_set *uset;
	isl_set *access_set;

	if (!group->private_bound)
		return;

	read = !strcmp(type, "read");

	access = group_access_relation(group, read, !read);
	access = isl_union_map_apply_domain(access, isl_union_map_copy(sched));
	access = isl_union_map_intersect(access,
				    isl_union_map_copy(gen->private_access));
	uset = isl_union_map_range(access);

	if (isl_union_set_is_empty(uset)) {
		isl_union_set_free(uset);
		return;
	}

	access_set = isl_union_set_copy_set(uset);
	isl_union_set_free(uset);
	access_set = isl_set_coalesce(access_set);
	access_set = isl_set_eliminate(access_set, isl_dim_param,
			first_shared + shared_len,
			gen->shared_len - shared_len);

	print_private_access(gen, shared_domain, access_set, type, group);
}

/* Print code for reading into or writing from private memory at
 * the given level (-1 for innermost).
 *
 * If we are not printing at the innermost level, then the dimensionality
 * of shared_domain may be smaller than gen->shared_len.
 * As the rest of the code assumes that the domain of access has
 * gen->shared_len dimensions, we therefore may need to embed this domain
 * in a higher dimensional space after intersection with shared_domain.
 *
 * This code is very similar to print_shared_accesses.
 * The main difference is that we to take into account gen->private_access.
 */
static void print_private_accesses(struct cuda_gen *gen,
	__isl_keep isl_set *shared_domain, __isl_keep isl_union_map *access,
	const char *type, int level)
{
	int i, j;
	isl_dim *dim;
	isl_map *proj;
	int shared_len = isl_set_dim(shared_domain, isl_dim_set);
	unsigned first_shared;
	isl_union_map *sched;

	shared_domain = isl_set_copy(shared_domain);
	sched = isl_union_map_copy(gen->tiled_sched);
	dim = isl_union_map_get_dim(sched);
	first_shared = isl_dim_size(dim, isl_dim_param);
	proj = projection(dim, gen->tiled_len, shared_len);
	sched = isl_union_map_apply_range(sched, isl_union_map_from_map(proj));
	sched = isl_union_map_intersect_range(sched,
			isl_union_set_from_set(isl_set_copy(shared_domain)));
	if (shared_len != gen->shared_len) {
		dim = isl_union_map_get_dim(sched);
		proj = projection(dim, gen->shared_len, shared_len);
		proj = isl_map_reverse(proj);
		shared_domain = isl_set_apply(shared_domain,
						isl_map_copy(proj));
		sched = isl_union_map_apply_range(sched,
				isl_union_map_from_map(proj));
	}

	for (i = 0; i < gen->n_array; ++i) {
		struct cuda_array_info *array = &gen->array[i];

		if (gen->array[i].print_shared_level != level)
			continue;

		for (j = 0; j < array->n_group; ++j)
			print_group_private_accesses(gen, array->groups[j],
					    type, shared_domain,
					    first_shared, shared_len, sched);
	}

	isl_union_map_free(sched);
	isl_set_free(shared_domain);
}

/* Set unroll[j] if the input dimension j is involved in
 * the index expression represented by bmap.
 */
static int check_unroll(__isl_take isl_basic_map *bmap, void *user)
{
	int i, j;
	int n_in = isl_basic_map_dim(bmap, isl_dim_in);
	int n_out = isl_basic_map_dim(bmap, isl_dim_out);
	int *unroll = user;

	for (i = 0; i < n_out; ++i) {
		isl_constraint *c;
		int ok;

		ok = isl_basic_map_has_defining_equality(bmap,
							isl_dim_out, i, &c);
		assert(ok);
		for (j = 0; j < n_in; ++j)
			if (isl_constraint_involves_dims(c, isl_dim_in, j, 1))
				unroll[j] = 1;
		isl_constraint_free(c);
	}

	isl_basic_map_free(bmap);
	return 0;
}

/* Given an array pos mapping input dimensions to the corresponding
 * output dimension, construct the corresponding map.
 */
static __isl_give isl_map *permutation(__isl_take isl_dim *dim,
	int *pos, int len)
{
	int i;
	isl_constraint *c;
	isl_basic_map *bmap;

	dim = isl_dim_add(dim, isl_dim_in, len);
	dim = isl_dim_add(dim, isl_dim_out, len);
	bmap = isl_basic_map_universe(isl_dim_copy(dim));

	for (i = 0; i < len; ++i) {
		c = isl_equality_alloc(isl_dim_copy(dim));
		isl_constraint_set_coefficient_si(c, isl_dim_in, i, -1);
		isl_constraint_set_coefficient_si(c, isl_dim_out, pos[i], 1);
		bmap = isl_basic_map_add_constraint(bmap, c);
	}
	isl_dim_free(dim);

	return isl_map_from_basic_map(bmap);
}

/* Find all loops involved in any of the index expressions for any of
 * the private accesses, move them innermost and then mark them as
 * requiring unrolling by setting gen->first_unroll.
 * The loops involved should all be parallel because of the checks
 * we performed in check_private_group_access.  Moving them innermost
 * is therefore a valid transformation.
 */
static __isl_give isl_union_map *interchange_for_unroll(struct cuda_gen *gen,
	__isl_take isl_union_map *sched)
{
	int i, j;
	int unroll[gen->thread_tiled_len];
	int perm[gen->thread_tiled_len];
	isl_dim *dim;
	isl_map *permute;
	int len = gen->shared_len + gen->n_parallel + gen->n_block;

	gen->first_unroll = -1;

	for (i = 0; i < gen->thread_tiled_len; ++i)
		unroll[i] = 0;
	for (i = 0; i < gen->n_array; ++i) {
		struct cuda_array_info *array = &gen->array[i];

		for (j = 0; j < array->n_group; ++j) {
			isl_union_map *access;
			isl_dim *dim;
			isl_map *acc;

			if (!array->groups[j]->private_bound)
				continue;

			access = group_access_relation(array->groups[j], 1, 1);
			access = isl_union_map_apply_domain(access,
						isl_union_map_copy(sched));

			dim = isl_union_map_get_dim(access);
			dim = isl_dim_add(dim, isl_dim_out, array->n_index);
			dim = isl_dim_set_tuple_name(dim, isl_dim_out,
							array->name);
			dim = isl_dim_add(dim, isl_dim_in,
							gen->thread_tiled_len);
			acc = isl_union_map_extract_map(access, dim);

			isl_map_foreach_basic_map(acc, &check_unroll, unroll);

			isl_map_free(acc);
			isl_union_map_free(access);
		}
	}

	for (i = 0; i < gen->shared_len; ++i)
		if (unroll[i])
			return sched;

	for (i = gen->shared_len; i < len; ++i)
		if (unroll[i])
			break;

	if (i >= len)
		return sched;

	for (i = len; i < gen->thread_tiled_len; ++i)
		if (unroll[i])
			return sched;

	j = 0;
	for (i = 0; i < gen->thread_tiled_len; ++i)
		if (!unroll[i])
			perm[i] = j++;
	gen->first_unroll = 1 + j;
	for (i = 0; i < len; ++i)
		if (unroll[i])
			perm[i] = j++;

	dim = isl_union_map_get_dim(sched);
	permute = permutation(dim, perm, gen->thread_tiled_len);
	sched = isl_union_map_apply_range(sched,
					  isl_union_map_from_map(permute));

	return sched;
}

/* This function is called for each leaf in the clast of the kernel code.
 * We first specialize the schedule to the site of the leaf and
 * print code for reading into shared memory, performing the actual
 * computations and writing from shared memory, with the required
 * synchronizations.
 */
static void print_kernel_user(struct gpucode_info *code,
	struct clast_user_stmt *u)
{
	struct cuda_gen *gen = code->user;
	isl_set *shared_domain;

	shared_domain = extract_entire_host_domain(u);

	print_shared_accesses(gen, shared_domain, gen->read, "read", -1);

	print_private_accesses(gen, shared_domain, gen->read, "read", -1);

	print_shared_body(gen, shared_domain, gen->local_sched,
			    gen->thread_tiled_len, &print_statement,
			    gen->first_unroll);

	print_private_accesses(gen, shared_domain, gen->write, "write", -1);

	print_indent(gen->cuda.kernel_c, gen->kernel_code.indent);
	fprintf(gen->cuda.kernel_c, "__syncthreads();\n");

	print_shared_accesses(gen, shared_domain, gen->write, "write", -1);

	isl_set_free(shared_domain);
}

/* Check if we need to perform any copying to shared memory at this level
 * and if so, print the copying instructions.
 * Any array for which we are allowed to print copying instructions at
 * this level, but haven't done so already, is printed.
 */
static void print_kernel_for_head(struct gpucode_info *code,
	struct clast_for *f)
{
	int i;
	struct cuda_gen *gen = code->user;
	isl_set *domain;
	int level;
	int print = 0;

	domain = isl_set_from_cloog_domain(cloog_domain_copy(f->domain));
	level = isl_set_dim(domain, isl_dim_set) - 1;

	for (i = 0; i < gen->n_array; ++i) {
		if (gen->array[i].print_shared_level >= 0)
			continue;
		if (gen->array[i].last_shared > level)
			continue;
		gen->array[i].print_shared_level = level;
		print = 1;
	}

	if (print) {
		print_shared_accesses(gen, domain, gen->read, "read", level);
		print_private_accesses(gen, domain, gen->read, "read", level);
	}

	isl_set_free(domain);
}

/* Print instructions for copying from shared memory for each array
 * for which print_kernel_for_head has added copying instructions
 * to shared memory.
 */
static void print_kernel_for_foot(struct gpucode_info *code,
	struct clast_for *f)
{
	int i;
	struct cuda_gen *gen = code->user;
	isl_set *domain;
	int level;
	int print = 0;

	domain = isl_set_from_cloog_domain(cloog_domain_copy(f->domain));
	level = isl_set_dim(domain, isl_dim_set) - 1;

	for (i = 0; i < gen->n_array; ++i) {
		if (gen->array[i].print_shared_level != level)
			continue;
		print = 1;
		break;
	}

	if (print) {
		print_private_accesses(gen, domain, gen->write, "write", level);
		print_shared_accesses(gen, domain, gen->write, "write", level);
	}

	isl_set_free(domain);
}

/* Use CLooG to generate code for the outer gen->shared_first loops
 * of the local schedule "sched".
 * The pretty printing of this code is handled by gpu_print_host_stmt,
 * which calls print_kernel_user for each iteration of the shared tile loops.
 */
static void print_cloog_kernel_body(struct cuda_gen *gen,
	__isl_keep isl_set *context, __isl_keep isl_union_map *sched)
{
	int i;
	CloogOptions *options;
	CloogDomain *cloog_context;
	CloogUnionDomain *ud;
	CloogInput *input;
	struct clast_stmt *stmt;
	char name[20];

	sched = isl_union_map_copy(sched);
	sched = isl_union_map_align_params(sched, isl_set_get_dim(context));

	options = cloog_options_malloc(gen->state);
	options->language = LANGUAGE_C;
	options->strides = 1;
	options->sh = 1;
	options->stop = gen->shared_len;
	options->f = gen->tiled_len;
	options->l = gen->tiled_len;
	options->save_domains = 1;
	options->noscalars = 1;

	ud = cloog_union_domain_from_isl_union_map(sched);
	for (i = 0; i < gen->shared_len; ++i) {
		snprintf(name, sizeof(name), "g%d", i);
		ud = cloog_union_domain_set_name(ud, CLOOG_SCAT, i, name);
	}
	cloog_context = cloog_domain_from_isl_set(isl_set_copy(context));
	input = cloog_input_alloc(cloog_context, ud);

	stmt = cloog_clast_create_from_input(input, options);

	gen->kernel_code.indent = 4;
	gen->kernel_code.dst = gen->cuda.kernel_c;
	gen->kernel_code.print_user_stmt = NULL;
	gen->kernel_code.print_user_stmt_list = &print_kernel_user;
	gen->kernel_code.print_for_head = &print_kernel_for_head;
	gen->kernel_code.print_for_foot = &print_kernel_for_foot;
	gen->kernel_code.user = gen;
	gpu_print_host_stmt(&gen->kernel_code, stmt);

	cloog_clast_free(stmt);
	cloog_options_free(options);
}

static void print_kernel_iterators(struct cuda_gen *gen)
{
	int i;
	const char *block_dims[] = { "blockIdx.x", "blockIdx.y" };
	const char *thread_dims[] = { "threadIdx.x", "threadIdx.y",
					"threadIdx.z" };

	if (gen->n_grid > 0) {
		print_indent(gen->cuda.kernel_c, 4);
		fprintf(gen->cuda.kernel_c, "int ");
		for (i = 0; i < gen->n_grid; ++i) {
			if (i)
				fprintf(gen->cuda.kernel_c, ", ");
			fprintf(gen->cuda.kernel_c, "b%d = %s",
				i, block_dims[gen->n_grid - 1 - i]);
		}
		fprintf(gen->cuda.kernel_c, ";\n");
	}

	if (gen->n_block > 0) {
		print_indent(gen->cuda.kernel_c, 4);
		fprintf(gen->cuda.kernel_c, "int ");
		for (i = 0; i < gen->n_block; ++i) {
			if (i)
				fprintf(gen->cuda.kernel_c, ", ");
			fprintf(gen->cuda.kernel_c, "t%d = %s",
				i, thread_dims[gen->n_block - 1 - i]);
		}
		fprintf(gen->cuda.kernel_c, ";\n");
	}
}

static void print_group_shared_array(struct cuda_gen *gen,
	struct cuda_array_ref_group *group)
{
	int j;
	struct cuda_array_bound *bounds;

	bounds = group->private_bound;
	if (!bounds)
		bounds = group->shared_bound;
	if (!bounds)
		return;

	print_indent(gen->cuda.kernel_c, 4);
	fprintf(gen->cuda.kernel_c, "%s%s ",
		group->private_bound ? "" : "__shared__ ", gen->options->type);
	print_array_name(gen->cuda.kernel_c, group);
	for (j = 0; j < group->array->n_index; ++j) {
		fprintf(gen->cuda.kernel_c, "[");
		isl_int_print(gen->cuda.kernel_c, bounds[j].size, 0);
		fprintf(gen->cuda.kernel_c, "]");
	}
	fprintf(gen->cuda.kernel_c, ";\n");
}

static void print_shared_arrays(struct cuda_gen *gen)
{
	int i, j;

	for (i = 0; i < gen->n_array; ++i) {
		struct cuda_array_info *array = &gen->array[i];

		for (j = 0; j < array->n_group; ++j)
			print_group_shared_array(gen, array->groups[j]);
	}
}

static void print_kernel_body(struct cuda_gen *gen,
	__isl_keep isl_set *host_domain, __isl_keep isl_union_map *sched)
{
	isl_set *context;

	context = isl_set_copy(host_domain);
	context = parametrize(context, 0, gen->tile_first, "h");
	context = isl_set_project_out(context, isl_dim_set, 0, gen->tile_first);
	context = add_bounded_parameters(context,
					gen->n_grid, gen->grid_dim, "b");

	print_kernel_iterators(gen);
	print_shared_arrays(gen);

	fprintf(gen->cuda.kernel_c, "\n");

	print_cloog_kernel_body(gen, context, sched);

	isl_set_free(context);
}

/* Given a constraint
 *
 *		a(p,i) + j = g f(e)
 *
 * or -a(p,i) - j = g f(e) if sign < 0,
 * store a(p,i) in bound->shift and g (stride) in bound->stride.
 * a(p,i) is assumed to be an expression in only the parameters.
 */
static void extract_stride(__isl_keep isl_constraint *c,
	struct cuda_array_bound *bound, isl_int stride, int sign)
{
	int i;
	isl_int v;
	isl_int one;
	isl_dim *dim;
	unsigned nparam;
	isl_qpolynomial *qp;

	isl_int_set(bound->stride, stride);

	dim = isl_constraint_get_dim(c);
	dim = isl_dim_drop(dim, isl_dim_out, 0, 1);
	dim = isl_dim_drop(dim, isl_dim_in, 0, isl_dim_size(dim, isl_dim_in));
	dim = isl_dim_domain(dim);

	nparam = isl_dim_size(dim, isl_dim_param);

	isl_int_init(v);
	isl_int_init(one);
	isl_int_set_si(one, 1);

	isl_constraint_get_constant(c, &v);
	if (sign < 0)
		isl_int_neg(v, v);
	qp = isl_qpolynomial_rat_cst(isl_dim_copy(dim), v, one);

	for (i = 0; i < nparam; ++i) {
		isl_qpolynomial *t, *p;

		isl_constraint_get_coefficient(c, isl_dim_param, i, &v);
		if (isl_int_is_zero(v))
			continue;
		if (sign < 0)
			isl_int_neg(v, v);
		t = isl_qpolynomial_rat_cst(isl_dim_copy(dim), v, one);
		p = isl_qpolynomial_var(isl_dim_copy(dim), isl_dim_param, i);
		t = isl_qpolynomial_mul(t, p);
		qp = isl_qpolynomial_add(qp, t);
	}

	isl_dim_free(dim);
	isl_int_clear(one);
	isl_int_clear(v);

	bound->shift = qp;
}

/* Given an equality constraint of a map with a single output dimension j,
 * check if the constraint is of the form
 *
 *		a(p,i) + j = g f(e)
 *
 * with a(p,i) an expression in the parameters and input dimensions
 * and f(e) an expression in the existentially quantified variables.
 * If so, and if g is larger than any such g from a previously considered
 * constraint, then call extract_stride. to record the stride information
 * in bound.
 */
static int check_stride_constraint(__isl_take isl_constraint *c, void *user)
{
	int i;
	isl_int v, stride;
	unsigned n_div;
	struct cuda_array_bound *bound = user;

	isl_int_init(v);
	isl_int_init(stride);

	n_div = isl_constraint_dim(c, isl_dim_div);
	isl_constraint_get_coefficient(c, isl_dim_out, 0, &v);

	if (n_div && (isl_int_is_one(v) || isl_int_is_negone(v))) {
		int s = isl_int_sgn(v);
		isl_int_set_si(stride, 0);
		for (i = 0; i < n_div; ++i) {
			isl_constraint_get_coefficient(c, isl_dim_div, i, &v);
			isl_int_gcd(stride, stride, v);
		}
		if (!isl_int_is_zero(stride) &&
		    isl_int_gt(stride, bound->stride))
			extract_stride(c, bound, stride, s);
	}

	isl_int_clear(stride);
	isl_int_clear(v);

	isl_constraint_free(c);
	return 0;
}

/* Given contraints on an array index i, check if we can find
 * a shift a(p) and a stride g such that
 *
 *	a(p) + i = 0 mod g
 *
 * If so, record the information in bound and apply the mapping
 * i -> (i + a(p))/g to the array index in bounds and return
 * the new constraints.
 * If not, simply return the original constraints.
 */
static __isl_give isl_basic_map *check_stride(struct cuda_gen *gen,
	struct cuda_array_bound *bound, __isl_take isl_basic_map *bounds)
{
	isl_dim *dim;
	isl_basic_map *aff;
	isl_basic_map *shift;
	isl_qpolynomial *qp, *t;
	isl_int one;

	isl_int_set_si(bound->stride, -1);

	aff = isl_basic_map_affine_hull(isl_basic_map_copy(bounds));

	isl_basic_map_foreach_constraint(aff, &check_stride_constraint, bound);

	isl_basic_map_free(aff);

	if (isl_int_is_neg(bound->stride))
		return bounds;

	qp = isl_qpolynomial_copy(bound->shift);
	qp = isl_qpolynomial_add_dims(qp, isl_dim_set, 1);
	dim = isl_qpolynomial_get_dim(qp);
	t = isl_qpolynomial_var(isl_dim_copy(dim), isl_dim_set, 0);
	qp = isl_qpolynomial_add(qp, t);
	isl_int_init(one);
	isl_int_set_si(one, 1);
	t = isl_qpolynomial_rat_cst(dim, one, bound->stride);
	isl_int_clear(one);
	qp = isl_qpolynomial_mul(qp, t);
	shift = isl_basic_map_from_qpolynomial(qp);

	bound->shift_map = isl_basic_map_copy(shift);
	bounds = isl_basic_map_apply_range(bounds, shift);

	return bounds;
}

struct cuda_size_info {
	isl_basic_set *bset;
	struct cuda_array_bound *bound;
	int pos;
};

/* Given a constraint from the basic set describing the bounds on
 * an array index, check if it is a lower bound, say m i >= b(x), and,
 * if so, check whether the expression "i - ceil(b(x)/m) + 1" has a constant
 * upper bound.  If so, and if this bound is smaller than any bound
 * derived from earlier constraints, set the size to this bound on
 * the expression and the lower bound to b(x).
 */
static int compute_size_in_direction(__isl_take isl_constraint *c, void *user)
{
	struct cuda_size_info *size = user;
	unsigned nparam;
	unsigned n_div;
	isl_int v;

	nparam = isl_basic_set_dim(size->bset, isl_dim_param);
	n_div = isl_constraint_dim(c, isl_dim_div);

	if (isl_constraint_involves_dims(c, isl_dim_div, 0, n_div)) {
		isl_constraint_free(c);
		return 0;
	}

	isl_int_init(v);

	isl_constraint_get_coefficient(c, isl_dim_set, size->pos, &v);

	if (isl_int_is_pos(v)) {
		isl_aff *aff;
		isl_qpolynomial *lb;
		enum isl_lp_result res;

		aff = isl_constraint_get_bound(c, isl_dim_set, size->pos);
		aff = isl_aff_ceil(aff);

		lb = isl_qpolynomial_from_aff(isl_aff_copy(aff));

		aff = isl_aff_neg(aff);
		aff = isl_aff_add_coefficient_si(aff, isl_dim_set, size->pos, 1);

		res = isl_basic_set_max(size->bset, aff, &v);
		isl_aff_free(aff);

		if (res == isl_lp_ok) {
			isl_int_add_ui(v, v, 1);
			if (isl_int_is_neg(size->bound->size) ||
			    isl_int_lt(v, size->bound->size)) {
				isl_int_set(size->bound->size, v);
				isl_qpolynomial_free(size->bound->lb);
				size->bound->lb = isl_qpolynomial_copy(lb);
			}
		}
		isl_qpolynomial_free(lb);
	}

	isl_int_clear(v);
	isl_constraint_free(c);

	return 0;
}

/* Given a basic map "bounds" that maps parameters and input dimensions
 * to a single output dimension, look for an expression in the parameters
 * and input dimensions such that the range of the output dimension shifted
 * by this expression is a constant.
 *
 * In particular, we currently only consider lower bounds on the output
 * dimension as candidate expressions.
 */
static int compute_array_dim_size(struct cuda_gen *gen,
	struct cuda_array_bound *bound, __isl_take isl_basic_map *bounds)
{
	struct cuda_size_info size;

	bounds = check_stride(gen, bound, bounds);

	isl_int_set_si(bound->size, -1);
	bound->lb = NULL;

	size.bound = bound;
	size.pos = isl_basic_map_dim(bounds, isl_dim_in);
	size.bset = isl_basic_map_wrap(bounds);
	size.bset = isl_basic_set_flatten(size.bset);
	isl_basic_set_foreach_constraint(size.bset, &compute_size_in_direction,
					&size);
	isl_basic_set_free(size.bset);

	return isl_int_is_nonneg(bound->size) ? 0 : -1;
}

/* Check if we can find a shared memory tile for the given array
 * based on the given accesses, and if so, put the results
 * in array->shared_bound.
 *
 * We project the accesses on each index in turn and look for a parametric
 * offset such that the size is constant.
 */
static int can_tile_for_shared_memory(struct cuda_gen *gen,
	struct cuda_array_info *array, __isl_keep isl_map *access,
	struct cuda_array_bound *bounds)
{
	int i;

	for (i = 0; i < array->n_index; ++i) {
		isl_map *access_i;
		isl_basic_map *hull;

		access_i = isl_map_copy(access);
		access_i = isl_map_project_out(access_i, isl_dim_out, 0, i);
		access_i = isl_map_project_out(access_i, isl_dim_out,
					    i + 1, array->n_index - (i + 1));
		access_i = isl_map_compute_divs(access_i);
		hull = isl_map_simple_hull(access_i);
		if (compute_array_dim_size(gen, &bounds[i], hull) < 0)
			return 0;
	}

	return 1;
}

/* Construct a map with input the shared tile loops and the loops that
 * will be wrapped around the threads that relates these later loops
 * to the thread indices and the projects them out.
 */
static __isl_give isl_map *compute_privatization(struct cuda_gen *gen)
{
	isl_map *priv;
	isl_map *tiling;
	isl_map *proj;
	isl_set *par;
	isl_dim *dim;

	dim = isl_union_map_get_dim(gen->shared_sched);

	if (gen->options->wrap)
		tiling = wrap(isl_dim_copy(dim), gen->shared_len + gen->n_block,
				gen->shared_len, gen->n_block, gen->block_dim);
	else
		tiling = tile(isl_dim_copy(dim), gen->shared_len + gen->n_block,
				gen->shared_len, gen->n_block, gen->block_dim);

	priv = tiling;

	par = parametrization(dim, gen->shared_len + 2 * gen->n_block,
		gen->tile_first + gen->tile_len + gen->n_grid + gen->n_block,
		gen->n_block, "t");

	priv = isl_map_align_params(priv, isl_set_get_dim(par));
	priv = isl_map_intersect_range(priv, par);

	dim = isl_map_get_dim(priv);
	dim = isl_dim_drop(dim, isl_dim_in, 0, isl_dim_size(dim, isl_dim_in));
	dim = isl_dim_drop(dim, isl_dim_out, 0, isl_dim_size(dim, isl_dim_out));
	proj = projection(dim, gen->shared_len + 2 * gen->n_block,
			  gen->shared_len);

	priv = isl_map_apply_range(priv, proj);

	return priv;
}

/* Construct a map from domain_dim to domain_dim that increments
 * the dimension at position "pos" and leaves all other dimensions
 * constant.
 */
static __isl_give isl_map *next(__isl_take isl_dim *domain_dim, int pos)
{
	int i;
	int len = isl_dim_size(domain_dim, isl_dim_set);
	isl_dim *dim;
	isl_basic_map *next;

	dim = isl_dim_map_from_set(domain_dim);
	next = isl_basic_map_universe(isl_dim_copy(dim));

	for (i = 0; i < len; ++i) {
		isl_constraint *c;

		c = isl_equality_alloc(isl_dim_copy(dim));
		isl_constraint_set_coefficient_si(c, isl_dim_in, i, 1);
		isl_constraint_set_coefficient_si(c, isl_dim_out, i, -1);
		if (i == pos)
			isl_constraint_set_constant_si(c, 1);
		next = isl_basic_map_add_constraint(next, c);
	}

	isl_dim_free(dim);

	return isl_map_from_basic_map(next);
}

/* Check if the given access is coalesced.
 * That is, check whether incrementing the dimension that will get
 * wrapped over the last thread index results in incrementing
 * the last array index.
 *
 * This function is only called for access relations without reuse.
 */
static int access_is_coalesced(struct cuda_gen *gen,
	__isl_keep isl_union_map *access)
{
	isl_dim *dim;
	isl_map *access_map;
	isl_map *next_thread_x;
	isl_map *next_element;
	isl_map *map;
	int coalesced;

	access = isl_union_map_copy(access);
	access = isl_union_map_apply_domain(access,
				isl_union_map_copy(gen->tiled_sched));
	access_map = isl_union_map_copy_map(access);
	isl_union_map_free(access);

	dim = isl_map_get_dim(access_map);
	dim = isl_dim_domain(dim);
	next_thread_x = next(dim, gen->shared_len + gen->n_block - 1);

	dim = isl_map_get_dim(access_map);
	dim = isl_dim_range(dim);
	next_element = next(dim, isl_dim_size(dim, isl_dim_set) - 1);

	map = isl_map_apply_domain(next_thread_x, isl_map_copy(access_map));
	map = isl_map_apply_range(map, access_map);

	coalesced = isl_map_is_subset(map, next_element);

	isl_map_free(next_element);
	isl_map_free(map);

	return coalesced;
}

/* For the given array reference group, check whether the access is private
 * to the thread.  That is, check that any given array element
 * is only accessed by a single thread.
 * We compute an access relation that maps the shared tile loop iterators
 * and the shared point loop iterators that will be wrapped over the
 * threads to the array elements.
 * We actually check that those iterators that will be wrapped
 * partition the array space.  This check is stricter than necessary
 * since several iterations may be mapped onto the same thread
 * and then they could be allowed to access the same memory elements,
 * but our check does not allow this situation.
 *
 * We also check that the index expression only depends on parallel
 * loops.  That way, we can move those loops innermost and unroll them.
 * Again, we use a test that is stricter than necessary.
 * We actually check whether the index expression only depends
 * on the iterators that are wrapped over the threads.
 * These are necessarily parallel, but there may be more parallel loops.
 *
 * Combining the injectivity of the first test with the single-valuedness
 * of the second test, we simply test for bijectivity.
 *
 * If it turns out we can use registers, we compute the private memory
 * tile size using can_tile_for_shared_memory, after introducing a dependence
 * on the thread indices.
 *
 * Before performing any of the above computations, we first check
 * if there is any reuse on the reference group.  If not, we simply
 * return.  If, moreover, the access is coalesced then we also remove
 * the shared memory tiling since we should just use global memory instead.
 */
static void check_private_group_access(struct cuda_gen *gen,
	struct cuda_array_ref_group *group)
{
	isl_map *acc;
	isl_union_map *access;
	int n_index = group->array->n_index;

	access = group_access_relation(group, 1, 1);
	if (isl_union_map_is_injective(access)) {
		if (group->shared_bound && access_is_coalesced(gen, access)) {
			free_bound_list(group->shared_bound, n_index);
			group->shared_bound = NULL;
		}
		isl_union_map_free(access);
		return;
	}
	access = isl_union_map_apply_domain(access,
					isl_union_map_copy(gen->shared_sched));

	acc = isl_union_map_copy_map(access);
	isl_union_map_free(access);

	if (!isl_map_is_bijective(acc)) {
		isl_map_free(acc);
		return;
	}

	group->private_bound = create_bound_list(gen->ctx, n_index);
	acc = isl_map_align_params(acc, isl_map_get_dim(gen->privatization));
	acc = isl_map_apply_domain(acc, isl_map_copy(gen->privatization));
	if (!can_tile_for_shared_memory(gen, group->array, acc,
					group->private_bound)) {
		free_bound_list(group->private_bound, n_index);
		group->private_bound = NULL;
	}

	isl_map_free(acc);
}

/* Look for the last shared tile loop that affects the offset of the
 * shared or private tile and store the result in array->last_shared.
 */
static void set_last_shared(struct cuda_gen *gen,
	struct cuda_array_ref_group *group)
{
	int i, j;
	struct cuda_array_bound *bounds;
	unsigned first_shared = gen->first_shared;
	int n_index = group->array->n_index;

	bounds = group->private_bound;
	if (!bounds)
		bounds = group->shared_bound;
	if (!bounds)
		return;

	for (j = gen->shared_len - 1; j >= 0; --j) {
		for (i = 0; i < n_index; ++i) {
			isl_qpolynomial *lb, *shift;

			lb = bounds[i].lb;
			if (isl_qpolynomial_involves_dims(lb, isl_dim_param,
							first_shared + j, 1))
				break;

			shift = bounds[i].shift;
			if (!shift)
				continue;
			if (isl_qpolynomial_involves_dims(shift, isl_dim_param,
							first_shared + j, 1))
				break;
		}
		if (i < n_index)
			break;
	}
	group->array->last_shared = j;
}

/* Compute the sizes of all private arrays for the current kernel,
 * as well as the offsets of the private pieces in the original arrays.
 * If we cannot or don't want to privatize a given array group,
 * we use the shared memory tile sizes computed in
 * compute_group_shared_bound instead.
 *
 * If a given Array only has a single reference group and if we have
 * been able to find a privated or shared tile,
 * we also look for the last shared tile loop that affects the offset
 * (and therefore the array tile) and store the result in array->last_shared.
 *
 * A privatized copy of all access relations from reference groups that
 * are mapped to private memory is stored in gen->privatization.
 */
static void compute_private_size(struct cuda_gen *gen)
{
	int i, j;
	isl_union_map *private;

	private = isl_union_map_empty(isl_union_map_get_dim(gen->shared_sched));

	for (i = 0; i < gen->n_array; ++i) {
		struct cuda_array_info *array = &gen->array[i];

		for (j = 0; j < array->n_group; ++j) {
			check_private_group_access(gen, array->groups[j]);

			if (!array->groups[j]->private_bound)
				continue;

			private = isl_union_map_union(private,
				group_access_relation(array->groups[j], 1, 1));
		}

		array->last_shared = gen->shared_len - 1;
		array->print_shared_level = -1;

		if (array->n_group != 1)
			continue;
		set_last_shared(gen, array->groups[0]);
	}

	if (isl_union_map_is_empty(private))
		isl_union_map_free(private);
	else {
		isl_union_map *priv;

		private = isl_union_map_apply_domain(private,
					isl_union_map_copy(gen->shared_sched));
		priv = isl_union_map_from_map(isl_map_copy(gen->privatization));
		private = isl_union_map_apply_domain(private, priv);
		gen->private_access = private;
	}
}

/* Fill up the groups array with singleton groups, i.e., one group
 * per reference, initializing the array, access, write and refs fields.
 * In particular the access field is initialized to the scheduled
 * access relation of the array reference.
 *
 * Return the number of elements initialized, i.e., the number of
 * active references in the current kernel.
 */
static int populate_array_references(struct cuda_gen *gen,
	struct cuda_array_info *array, __isl_keep isl_union_map *sched,
	struct cuda_array_ref_group **groups)
{
	int i;
	int n;
	isl_ctx *ctx = isl_union_map_get_ctx(sched);

	n = 0;
	for (i = 0; i < array->n_ref; ++i) {
		isl_union_map *umap;
		isl_map *map;
		struct cuda_array_ref_group *group;
		struct cuda_stmt_access *access = array->refs[i];

		map = isl_map_copy(access->access);
		umap = isl_union_map_from_map(map);
		umap = isl_union_map_apply_domain(umap,
				isl_union_map_copy(sched));

		map = isl_union_map_copy_map(umap);
		isl_union_map_free(umap);

		if (isl_map_is_empty(map)) {
			isl_map_free(map);
			continue;
		}

		group = isl_calloc_type(ctx, struct cuda_array_ref_group);
		assert(group);
		group->array = array;
		group->access = map;
		group->write = access->write;
		group->refs = &array->refs[i];

		groups[n++] = group;
	}

	return n;
}

static void free_array_ref_group(struct cuda_array_ref_group *group,
	int n_index)
{
	if (!group)
		return;
	free_bound_list(group->shared_bound, n_index);
	free_bound_list(group->private_bound, n_index);
	isl_map_free(group->access);
	free(group->refs);
	free(group);
}

/* If two groups have overlapping access relations and if one of them
 * involves a write, then merge the two groups into one.
 *
 * We keep track of the grouping in "leader".  leader[j] points to
 * an earlier group array element that belongs to the same group,
 * or the array element j itself if this element is the first in the group.
 *
 * Return the number of group leaders.
 */
static int group_overlapping_writes(int n,
	struct cuda_array_ref_group **groups, int *leader)
{
	int i, j;
	int n_group = n;

	for (i = 0; i < n; ++i) {
		int l = i;
		groups[l]->n_ref = 1;
		for (j = i - 1; j >= 0; --j) {
			isl_map *map;
			int empty;

			if (leader[j] != j)
				continue;
			if (!groups[l]->write && !groups[j]->write)
				continue;

			map = isl_map_intersect(isl_map_copy(groups[l]->access),
					    isl_map_copy(groups[j]->access));
			empty = isl_map_is_empty(map);
			isl_map_free(map);

			if (empty)
				continue;
			
			groups[j]->access = isl_map_union(groups[j]->access,
							groups[l]->access);
			groups[j]->write = 1;
			groups[l]->access = NULL;
			groups[j]->n_ref += groups[l]->n_ref;
			l = leader[l] = j;
			n_group--;
		}
		leader[i] = l;
	}

	return n_group;
}

/* Compute the size of the shared array corresponding to the given array
 * array refrence group, based on the accesses from the current kernel,
 * as well as the offset of the shared piece in the original array.
 */
static void compute_group_shared_bound(struct cuda_gen *gen,
	struct cuda_array_info *array, struct cuda_array_ref_group *group)
{
	isl_ctx *ctx = isl_dim_get_ctx(array->dim);

	group->shared_bound = create_bound_list(ctx, array->n_index);
	if (!can_tile_for_shared_memory(gen, array, group->access,
					group->shared_bound)) {
		free_bound_list(group->shared_bound, array->n_index);
		group->shared_bound = NULL;
	}
}

/* Given an initial grouping of array references and shared memory tiles
 * for each group that allows for a shared memory tile, merge two groups
 * if both have a shared memory tile and if the merged group also has
 * a shared memory tile.
 *
 * Return the number of group leaders after merging.
 */
static int group_common_shared_memory_tile(struct cuda_gen *gen,
	struct cuda_array_info *array, int n,
	struct cuda_array_ref_group **groups, int *leader, int n_group)
{
	int i, j;
	isl_ctx *ctx = isl_dim_get_ctx(array->dim);

	for (i = 0; n_group > 1 && i < n; ++i) {
		int l = i;
		if (leader[i] != i)
			continue;
		if (!groups[i]->shared_bound)
			continue;
		for (j = i - 1; j >= 0; --j) {
			isl_map *map;
			int empty;
			struct cuda_array_bound *shared_bound;

			if (leader[j] != j)
				continue;
			if (!groups[j]->shared_bound)
				continue;

			map = isl_map_intersect(isl_map_copy(groups[l]->access),
					    isl_map_copy(groups[j]->access));
			empty = isl_map_is_empty(map);
			isl_map_free(map);

			if (empty)
				continue;

			map = isl_map_union(isl_map_copy(groups[l]->access),
					    isl_map_copy(groups[j]->access));
			shared_bound = create_bound_list(ctx, array->n_index);
			if (!can_tile_for_shared_memory(gen, array, map,
							shared_bound)) {
				isl_map_free(map);
				free_bound_list(shared_bound, array->n_index);
				continue;
			}

			free_bound_list(groups[j]->shared_bound,
					array->n_index);
			groups[j]->shared_bound = shared_bound;
			isl_map_free(groups[j]->access);
			groups[j]->access = map;
			groups[j]->n_ref += groups[l]->n_ref;
			l = leader[l] = j;
			n_group--;
		}
	}

	return n_group;
}

/* Extract an array of array reference groups from the array of references
 * and the grouping information in "leader".
 *
 * Store the results in array->n_group and array->groups.
 */
static void extract_array_groups(isl_ctx *ctx, struct cuda_array_info *array,
	int n, struct cuda_array_ref_group **groups, int *leader, int n_group)
{
	int i, j;

	for (i = 2; i < n; ++i)
		leader[i] = leader[leader[i]];

	array->n_group = n_group;
	array->groups = isl_alloc_array(ctx, struct cuda_array_ref_group *,
					n_group);
	assert(array->groups);

	j = 0;
	for (i = 0; i < n; ++i) {
		int k, l;
		struct cuda_stmt_access **refs;

		if (leader[i] != i) {
			groups[i]->refs = NULL;
			free_array_ref_group(groups[i], array->n_index);
			continue;
		}

		refs = isl_alloc_array(ctx, struct cuda_stmt_access *,
					groups[i]->n_ref);
		assert(refs);
		l = 0;
		for (k = i; k < n; ++k)
			if (leader[k] == i) {
				refs[l++] = *groups[k]->refs;
				(*groups[k]->refs)->group = j;
			}

		groups[i]->refs = refs;
		groups[i]->nr = j;
		array->groups[j++] = groups[i];
	}
}

/* Group array references that should be considered together when
 * deciding whether to access them from private, shared or global memory.
 *
 * In particular, if two array references overlap and if one of them
 * is a write, then the two references are grouped together.
 * Furthermore, if two groups admit a shared memory tile and if the
 * combination of the two also admits a shared memory tile, we merge
 * the two groups.
 *
 * During the construction the group->refs field points to a single
 * array reference inside the array of array references, while
 * group->n_ref contains the number of element in leader that
 * (directly or indirectly) point to this group, provided the group
 * is a leader.
 */
static void group_array_references(struct cuda_gen *gen,
	struct cuda_array_info *array, __isl_keep isl_union_map *sched)
{
	int i;
	int n, n_group;
	isl_ctx *ctx = isl_union_map_get_ctx(sched);
	struct cuda_array_ref_group **groups;
	int *leader;

	groups = isl_calloc_array(ctx, struct cuda_array_ref_group *,
					array->n_ref);
	assert(groups);

	n = populate_array_references(gen, array, sched, groups);

	leader = isl_alloc_array(ctx, int, n);
	assert(leader);

	n_group = group_overlapping_writes(n, groups, leader);

	for (i = 0; i < n; ++i)
		if (leader[i] == i)
			compute_group_shared_bound(gen, array, groups[i]);

	n_group = group_common_shared_memory_tile(gen, array, n, groups,
						  leader, n_group);

	extract_array_groups(ctx, array, n, groups, leader, n_group);

	free(leader);
	free(groups);
}

/* Take tiled_sched, project it onto the shared tile loops and
 * the loops that will be wrapped over the threads,
 * parametrize the shared tile loops and store the result in gen->shared_sched.
 * The position of the first of these parameters is stored in gen->first_shared.
 * Also compute a projection that projects out the loops that will be
 * wrapped over the threads and store this projection in gen->shared_proj.
 */
static void compute_shared_sched(struct cuda_gen *gen)
{
	isl_dim *dim;
	isl_map *proj;
	isl_set *par;
	isl_union_map *sched;

	sched = isl_union_map_copy(gen->tiled_sched);

	dim = isl_union_map_get_dim(sched);
	gen->first_shared = isl_dim_size(dim, isl_dim_param);
	proj = projection(dim, gen->tiled_len, gen->shared_len + gen->n_block);
	sched = isl_union_map_apply_range(sched, isl_union_map_from_map(proj));

	dim = isl_union_map_get_dim(sched);
	par = parametrization(dim, gen->shared_len + gen->n_block,
				0, gen->shared_len, "g");
	sched = isl_union_map_intersect_range(sched,
						isl_union_set_from_set(par));

	dim = isl_union_map_get_dim(sched);
	proj = projection(dim, gen->shared_len + gen->n_block, gen->shared_len);

	gen->shared_sched = sched;
	gen->shared_proj = isl_union_map_from_map(proj);
}

/* Group references of all arrays in the program.
 */
static void group_references(struct cuda_gen *gen)
{
	int i;
	isl_union_map *sched;

	sched = isl_union_map_apply_range(isl_union_map_copy(gen->shared_sched),
					  isl_union_map_copy(gen->shared_proj));

	for (i = 0; i < gen->n_array; ++i)
		group_array_references(gen, &gen->array[i], sched);

	isl_union_map_free(sched);
}

/* Free all array information that is local to the current kernel.
 */
static void free_local_array_info(struct cuda_gen *gen)
{
	int i, j;

	for (i = 0; i < gen->n_array; ++i) {
		struct cuda_array_info *array = &gen->array[i];

		for (j = 0; j < array->n_group; ++j)
			free_array_ref_group(array->groups[j], array->n_index);
		free(array->groups);

		if (array->n_group == 0)
			continue;
		for (j = 0; j < gen->array[i].n_index; ++j) {
			isl_pw_qpolynomial_fold_free(gen->array[i].local_bound[j]);
			gen->array[i].local_bound[j] = NULL;
		}
	}
}

static void print_iterator_list(FILE *out, int len, const char *prefix,
	int parens)
{
	int i;

	fprintf(out, "(");
	for (i = 0; i < len; ++i) {
		if (i)
			fprintf(out, ", ");
		if (parens)
			fprintf(out, "(%s%d)", prefix, i);
		else
			fprintf(out, "%s%d", prefix, i);
	}
	fprintf(out, ")");
}

/* Print an access to the element in the global memory copy of the
 * given array that corresponds to element [a0][a1]... of the original array.
 * The copy in global memory has been linearized, so we need to take
 * the array size into account.
 */
static void print_global_index(isl_ctx *ctx, FILE *out,
	struct cuda_array_info *array)
{
	int i;
	isl_printer *prn;

	fprintf(out, "%s[", array->name);
	for (i = 0; i + 1 < array->n_index; ++i)
		fprintf(out, "(");
	for (i = 0; i < array->n_index; ++i) {
		if (i) {
			prn = isl_printer_to_file(ctx, out);
			prn = isl_printer_set_output_format(prn, ISL_FORMAT_C);
			prn = isl_printer_print_str(prn, ") * (");
			prn = isl_printer_print_pw_qpolynomial_fold(prn,
							array->local_bound[i]);
			prn = isl_printer_print_str(prn, ") + ");
			isl_printer_free(prn);
		}
		fprintf(out, "a%d", i);
	}
	fprintf(out, "]");
}

/* Print an access to the element in the shared memory copy of the
 * given array that corresponds to element [a0][a1]... of the original array.
 * Since the array in shared memory is just a shifted copy of part
 * of the original array, we simply need to subtract the lower bound,
 * which was computed in can_tile_for_shared_memory.
 * If any of the indices is strided, then we first add
 * shared_bound[i].shift and divide by shared_bound[i].stride.
 */
static void print_local_index(FILE *out, struct cuda_array_ref_group *group)
{
	int i;
	isl_ctx *ctx;
	isl_printer *prn;
	struct cuda_array_bound *bounds = group->shared_bound;

	ctx = isl_dim_get_ctx(group->array->dim);
	print_array_name(out, group);
	for (i = 0; i < group->array->n_index; ++i) {
		fprintf(out, "[(a%d", i);
		if (bounds[i].shift) {
			fprintf(out, " + (");
			prn = isl_printer_to_file(ctx, out);
			prn = isl_printer_set_output_format(prn, ISL_FORMAT_C);
			prn = isl_printer_print_qpolynomial(prn,
						bounds[i].shift);
			prn = isl_printer_print_str(prn, "))/");
			prn = isl_printer_print_isl_int(prn,
						bounds[i].stride);
			isl_printer_free(prn);
		} else
			fprintf(out, ")");
		fprintf(out, " - (");
		prn = isl_printer_to_file(ctx, out);
		prn = isl_printer_set_output_format(prn, ISL_FORMAT_C);
		prn = isl_printer_print_qpolynomial(prn, bounds[i].lb);
		isl_printer_free(prn);
		fprintf(out, ")]");
	}
}

/* Print '#define's for copying data from global memory to shared
 * memory and back for the given array.
 */
static void print_array_copy_defines(struct cuda_gen *gen,
	struct cuda_array_ref_group *group)
{
	int i;
	const char *type[] = { "read", "write" };
	struct cuda_array_info *array = group->array;
	int n_index = array->n_index;

	for (i = 0; i < 2; ++i) {
		fprintf(gen->cuda.kernel_c, "#define %s_", type[i]);
		print_array_name(gen->cuda.kernel_c, group);
		print_iterator_list(gen->cuda.kernel_c, n_index, "a", 0);
		fprintf(gen->cuda.kernel_c, " %s_", type[i]);
		print_array_name(gen->cuda.kernel_c, group);
		fprintf(gen->cuda.kernel_c, "_");
		print_iterator_list(gen->cuda.kernel_c, n_index, "a", 1);
		fprintf(gen->cuda.kernel_c, "\n");

		fprintf(gen->cuda.kernel_c, "#define %s_", type[i]);
		print_array_name(gen->cuda.kernel_c, group);
		fprintf(gen->cuda.kernel_c, "_");
		print_iterator_list(gen->cuda.kernel_c, n_index, "a", 0);
		if (i) {
			fprintf(gen->cuda.kernel_c, " ");
			print_global_index(gen->ctx, gen->cuda.kernel_c, array);
			fprintf(gen->cuda.kernel_c, " = ");
			print_local_index(gen->cuda.kernel_c, group);
		} else {
			fprintf(gen->cuda.kernel_c, " ");
			print_local_index(gen->cuda.kernel_c, group);
			fprintf(gen->cuda.kernel_c, " = ");
			print_global_index(gen->ctx, gen->cuda.kernel_c, array);
		}
		fprintf(gen->cuda.kernel_c, "\n");
	}
}

static void print_copy_defines(struct cuda_gen *gen)
{
	int i, j;

	for (i = 0; i < gen->n_array; ++i) {
		struct cuda_array_info *array = &gen->array[i];

		for (j = 0; j < array->n_group; ++j) {
			if (array->groups[j]->private_bound)
				continue;
			if (!array->groups[j]->shared_bound)
				continue;
			print_array_copy_defines(gen, array->groups[j]);
		}
	}
}

/* The sizes of the arrays on the host that have been computed by
 * extract_array_info may depend on the parameters.  Use the extra
 * constraints on the parameters that are valid at "host_domain"
 * to simplify these expressions.
 */
static void localize_bounds(struct cuda_gen *gen,
	__isl_keep isl_set *host_domain)
{
	int i, j;
	isl_set *context;
	unsigned nvar;

	context = isl_set_copy(host_domain);
	nvar = isl_set_dim(host_domain, isl_dim_set);
	context = isl_set_project_out(host_domain, isl_dim_set, 0, nvar);

	for (i = 0; i < gen->n_array; ++i) {
		struct cuda_array_info *array = &gen->array[i];

		if (array->n_group == 0)
			continue;

		for (j = 0; j < array->n_index; ++j) {
			isl_pw_qpolynomial_fold *pwf;

			pwf = isl_pw_qpolynomial_fold_copy(array->bound[j]);
			pwf = isl_pw_qpolynomial_fold_gist(pwf,
							isl_set_copy(context));
			array->local_bound[j] = pwf;
		}
	}
	isl_set_free(context);
}

/* Set gen->tile_len and gen->n_parallel to those of the first statement
 * in the statement list u.
 * Because of the way the schedule is constructed, the other statements
 * in the list, if any, should have the same values for these properties.
 */
static void set_tile_len(struct cuda_gen *gen, struct clast_user_stmt *u)
{
	int nr;
	struct cuda_stmt *stmt;

	nr = atoi(u->statement->name + 2);
	stmt = &gen->stmts[nr];

	gen->tile_len = stmt->tile_len;
	gen->n_parallel = stmt->n_parallel;
}

/* This function is called for each leaf in the clast of the host code.
 * We first specialize the schedule to the site of the leaf, compute
 * the size of shared memory and then print the body of host code
 * and the associated kernel (through a call to print_kernel_body).
 */
static void print_host_user(struct gpucode_info *code,
	struct clast_user_stmt *u)
{
	struct cuda_gen *gen = code->user;
	isl_dim *dim;
	isl_set *par;
	isl_set *host_domain;
	isl_union_map *access;
	isl_union_map *local_sched;
	isl_union_set *arrays;

	set_tile_len(gen, u);
	read_sizes(gen);

	host_domain = extract_entire_host_domain(u);

	local_sched = isl_union_map_intersect_range(
		    isl_union_map_copy(gen->sched),
		    isl_union_set_from_set(extend(isl_set_copy(host_domain),
						  gen->untiled_len)));
	access = isl_union_map_union(isl_union_map_copy(gen->read),
				     isl_union_map_copy(gen->write));
	access = isl_union_map_apply_domain(access,
					    isl_union_map_copy(local_sched));
	arrays = isl_union_map_range(access);

	print_indent(code->dst, code->indent);
	fprintf(code->dst, "dim3 k%d_dimBlock(", gen->kernel_id);
	print_reverse_list(code->dst, gen->n_block, gen->block_dim);
	fprintf(code->dst, ");\n");

	print_indent(code->dst, code->indent);
	fprintf(code->dst, "dim3 k%d_dimGrid(", gen->kernel_id);
	print_reverse_list(code->dst, gen->n_grid, gen->grid_dim);
	fprintf(code->dst, ");\n");

	gen->tiled_sched = tile_schedule(gen, local_sched);
	gen->tiled_sched = parametrize_tiled_schedule(gen, gen->tiled_sched);
	gen->tiled_sched = scale_tile_loops(gen, gen->tiled_sched);

	gen->local_sched = isl_union_map_copy(gen->tiled_sched);

	dim = isl_union_map_get_dim(gen->local_sched);
	par = parametrization(dim, gen->tiled_len, 0, gen->shared_len, "g");
	gen->local_sched = isl_union_map_intersect_range(gen->local_sched,
						isl_union_set_from_set(par));

	gen->local_sched = thread_tile_schedule(gen, gen->local_sched);
	gen->local_sched = scale_thread_tile_loops(gen, gen->local_sched);

	gen->private_access = NULL;
	compute_shared_sched(gen);
	gen->privatization = compute_privatization(gen);
	group_references(gen);
	compute_private_size(gen);
	localize_bounds(gen, host_domain);

	gen->local_sched = interchange_for_unroll(gen, gen->local_sched);

	print_copy_defines(gen);
	print_kernel_launch(gen, arrays);

	fprintf(gen->cuda.kernel_c, "{\n");

	print_kernel_body(gen, host_domain, gen->tiled_sched);

	fprintf(gen->cuda.kernel_c, "}\n");

	free_local_array_info(gen);
	isl_map_free(gen->privatization);
	isl_union_map_free(gen->private_access);
	isl_union_map_free(gen->local_sched);
	isl_union_map_free(gen->tiled_sched);
	isl_union_map_free(gen->shared_sched);
	isl_union_map_free(gen->shared_proj);
	isl_union_set_free(arrays);
	isl_set_free(host_domain);

	free(gen->tile_size);
	gen->kernel_id++;
}

/* Use CLooG to generate code for the outer gen->tile_first loops
 * of the global schedule in gen->sched.
 * The pretty printing of this code is handled by gpu_print_host_stmt,
 * which calls print_host_user for each kernel invocation location.
 */
static void print_cloog_host_code(struct cuda_gen *gen)
{
	int i;
	isl_set *context;
	isl_union_map *sched;
	CloogOptions *options;
	CloogDomain *cloog_context;
	CloogUnionDomain *ud;
	CloogInput *input;
	struct clast_stmt *stmt;
	char name[20];

	options = cloog_options_malloc(gen->state);
	options->language = LANGUAGE_C;
	options->otl = 0;
	options->strides = 1;
	options->stop = gen->tile_first;
	options->f = gen->untiled_len;
	options->l = gen->untiled_len;
	options->save_domains = 1;
	options->noscalars = 1;

	sched = isl_union_map_copy(gen->sched);
	ud = cloog_union_domain_from_isl_union_map(sched);
	for (i = 0; i < options->stop; ++i) {
		snprintf(name, sizeof(name), "h%d", i);
		ud = cloog_union_domain_set_name(ud, CLOOG_SCAT, i, name);
	}
	context = isl_set_copy(gen->context);
	cloog_context = cloog_domain_from_isl_set(context);
	input = cloog_input_alloc(cloog_context, ud);

	stmt = cloog_clast_create_from_input(input, options);

	gen->code.indent = 0;
	gen->code.dst = gen->cuda.host_c;
	gen->code.print_user_stmt = NULL;
	gen->code.print_user_stmt_list = &print_host_user;
	gen->code.print_for_head = NULL;
	gen->code.print_for_foot = NULL;
	gen->code.user = gen;
	gpu_print_host_stmt(&gen->code, stmt);

	cloog_clast_free(stmt);
	cloog_options_free(options);
}

void print_host_code(struct cuda_gen *gen)
{
	fprintf(gen->cuda.host_c, "{\n");
	print_cloog_macros(gen->cuda.host_c);
	print_cloog_macros(gen->cuda.kernel_c);

	declare_device_arrays(gen);

	allocate_device_arrays(gen);
	copy_arrays_to_device(gen);

	gen->kernel_id = 0;
	print_cloog_host_code(gen);

	copy_arrays_from_device(gen);
	free_device_arrays(gen);

	fprintf(gen->cuda.host_c, "}\n");
}

__isl_give isl_set *add_context_from_str(__isl_take isl_set *set,
	const char *str)
{
	isl_ctx *ctx;
	isl_set *context;

	if (!str)
		return set;

	ctx = isl_set_get_ctx(set);
	context = isl_set_read_from_str(ctx, str, -1);
	context = isl_set_align_params(context, isl_set_get_dim(set));
	set = isl_set_intersect(set, context);

	return set;
}

/* Convert scop->context to an isl_set.
 */
static __isl_give isl_set *extract_context(isl_ctx *ctx, scoplib_scop_p scop)
{
	isl_dim *dim;

	dim = isl_dim_set_alloc(ctx, scop->nb_parameters, 0);
	dim = set_dim_names(dim, isl_dim_param, scop->parameters);
	return scoplib_matrix_to_isl_set(scop->context, dim);
}

/* Return an array of cuda_stmt representing the statements in "scop".
 */
static struct cuda_stmt *extract_stmts(isl_ctx *ctx, scoplib_scop_p scop,
	__isl_keep isl_set *context)
{
	int n;
	struct cuda_stmt *stmts;
	scoplib_statement_p stmt = scop->statement;

	n = scoplib_statement_number(scop->statement);
	stmts = isl_calloc_array(ctx, struct cuda_stmt, n);
	assert(stmts);

	for (stmt = scop->statement, n = 0; stmt; stmt = stmt->next, n++) {
		char name[20];
		isl_dim *dim;
		struct cuda_stmt *s = &stmts[n];

		snprintf(name, sizeof(name), "S_%d", n);

		dim = isl_dim_set_alloc(ctx, scop->nb_parameters,
					stmt->nb_iterators);
		dim = set_dim_names(dim, isl_dim_param, scop->parameters);
		dim = set_dim_names(dim, isl_dim_set, stmt->iterators);
		dim = isl_dim_set_tuple_name(dim, isl_dim_set, name);
		dim = set_dim_names(dim, isl_dim_set, stmt->iterators);
		s->domain = scoplib_matrix_list_to_isl_set(stmt->domain,
									dim);
		s->domain = isl_set_intersect(s->domain, isl_set_copy(context));
		s->text = strdup(stmt->body);
		stmt_extract_accesses(s);
	}

	return stmts;
}

/* Extract all the read and write accesses from "scop" and store
 * them in gen->read and gen->write.
 */
static void extract_accesses(struct cuda_gen *gen, scoplib_scop_p scop)
{
	int i;
	int n = scoplib_statement_number(scop->statement);
	isl_dim *dim;
	scoplib_statement_p stmt;

	dim = isl_set_get_dim(gen->context);
	gen->write = isl_union_map_empty(isl_dim_copy(dim));
	gen->read = isl_union_map_empty(dim);

	for (i = 0, stmt = scop->statement; i < n; ++i, stmt = stmt->next) {
		isl_union_map *read_i;
		isl_union_map *write_i;

		read_i = scoplib_access_to_isl_union_map(stmt->read,
				isl_set_copy(gen->stmts[i].domain),
				scop->arrays);
		write_i = scoplib_access_to_isl_union_map(stmt->write,
				isl_set_copy(gen->stmts[i].domain),
				scop->arrays);

		gen->read = isl_union_map_union(gen->read, read_i);
		gen->write = isl_union_map_union(gen->write, write_i);
	}
}

/* Extract and return the original schedule of the program from "scop".
 */
static isl_union_map *extract_original_schedule(struct cuda_gen *gen,
	scoplib_scop_p scop)
{
	int i;
	int n = scoplib_statement_number(scop->statement);
	isl_dim *dim;
	isl_union_map *sched;
	scoplib_statement_p stmt;

	dim = isl_set_get_dim(gen->context);
	sched = isl_union_map_empty(dim);

	for (i = 0, stmt = scop->statement; i < n; ++i, stmt = stmt->next) {
		isl_map *sched_i;

		dim = isl_set_get_dim(gen->stmts[i].domain);
		dim = isl_dim_from_domain(dim);
		dim = isl_dim_add(dim, isl_dim_out, 2 * stmt->nb_iterators + 1);
		sched_i = scoplib_schedule_to_isl_map(stmt->schedule, dim);

		sched = isl_union_map_union(sched,
					    isl_union_map_from_map(sched_i));
	}

	return sched;
}

/* Return the union of all iteration domains of the gen->stmts[i].
 */
static __isl_give isl_union_set *extract_domain(struct cuda_gen *gen)
{
	int i;
	isl_union_set *domain;

	domain = isl_union_set_empty(isl_set_get_dim(gen->context));
	for (i = 0; i < gen->n_stmts; ++i) {
		isl_set *domain_i;

		domain_i = isl_set_copy(gen->stmts[i].domain);
		domain = isl_union_set_union(domain,
					     isl_union_set_from_set(domain_i));
	}

	return domain;
}

/* Information about the outermost tilable bands in the forest of bands.
 *
 * tile_len and n_parallel are only sets on band_info structures
 * that correspond to outermost bands.  For other bands (in particular,
 * ancestors of the outermost bands), n_parallal is set to 0.
 *
 * prefix is the (padded) schedule leading up to the outermost tilable bands.
 *
 * tile_first is the number of schedule dimensions in prefix.
 *
 * suffix is the schedule of the outermost tilable bands and their descendants.
 */
struct band_info {
	struct cuda_gen *gen;
	int tile_first;
	int tile_len;
	int n_parallel;
	isl_union_map *prefix;
	isl_union_map *suffix;
};

/* Set tile_len and n_parallel of the statement to that of
 * their outermost band, recorded in the band_info.
 */
static int set_stmt_tile_len(__isl_take isl_map *map, void *user)
{
	struct band_info *info = user;
	int nr;
	struct cuda_stmt *stmt;

	nr = atoi(isl_map_get_tuple_name(map, isl_dim_in) + 2);
	stmt = &info->gen->stmts[nr];

	stmt->tile_len = info->tile_len;
	stmt->n_parallel = info->n_parallel;

	isl_map_free(map);

	return 0;
}

static void list_select_outer_band(struct cuda_gen *gen,
	__isl_take isl_band_list *list, int pos, struct band_info *list_info);

/* Check if this band has any parallel loops.  If so, take it as
 * the outermost tilable band.  If not, continue looking for the
 * outermost tilable band in the children of the current band.
 */
static void band_select_outer_band(struct cuda_gen *gen,
	__isl_take isl_band *band, int pos, struct band_info *info)
{
	int n = isl_band_n_member(band);
	int n_parallel;

	for (n_parallel = 0; n_parallel < n; ++n_parallel)
		if (!isl_band_member_is_zero_distance(band, n_parallel))
			break;

	info->n_parallel = n_parallel;
	if (n_parallel) {
		info->gen = gen;
		info->tile_first = pos;
		info->tile_len = n;
		info->prefix = isl_band_get_prefix_schedule(band);
		info->suffix = isl_union_map_flat_range_product(
				isl_band_get_partial_schedule(band),
				isl_band_get_suffix_schedule(band));
		isl_union_map_foreach_map(info->prefix,
					    &set_stmt_tile_len, info);
	} else {
		isl_band_list *children;
		assert(isl_band_has_children(band));
		children = isl_band_get_children(band);
		list_select_outer_band(gen, children, pos + n, info);
	}

	isl_band_free(band);
}

/* Comparison function that returns a non-zero value for band_infos
 * with different tile_len fields or different n_parallel fields.
 */
static int cmp_band(const void *p1, const void *p2)
{
	const struct band_info *info1 = p1;
	const struct band_info *info2 = p2;

	if (info1->tile_len != info2->tile_len)
		return info1->tile_len - info2->tile_len;

	return info1->n_parallel - info2->n_parallel;
}

/* Extend "umap" with coordinates with fixed value "val"
 * to a total length of "dst_len", assuming the original dimension is "src_len".
 */
static __isl_give isl_union_map *extend_range(__isl_take isl_union_map *umap,
	int src_len, int dst_len, int val)
{
	isl_dim *dim;
	isl_map *map;
	int i;

	dim = isl_union_map_get_dim(umap);
	map = isl_map_reverse(projection(dim, dst_len, src_len));
	for (i = src_len; i < dst_len; ++i)
		map = isl_map_fix_si(map, isl_dim_out, i, val);

	umap = isl_union_map_apply_range(umap, isl_union_map_from_map(map));

	return umap;
}

/* Group bands with the same values for tile_len and n_parallel.
 * The prefix schedule is then extended with a fixed coordinate that
 * is different for each such group.
 * Note that the actual values for this coordinate are not important.
 * The bands have already been effectively separated at a higher level
 * or they are independent and may be executed in parallel.
 * The list of band_info has been sorted before this functions is called.
 */
static void separate_bands(struct band_info *info, int n)
{
	int i;
	int j = 0;

	for (i = 0; i < n; ++i) {
		int l = info[i].tile_first;

		if (i &&
		    (info[i].tile_len != info[i - 1].tile_len ||
		     info[i].n_parallel != info[i - 1].n_parallel))
			j++;

		info[i].prefix = extend_range(info[i].prefix,
						l, l + 1, j);
		info[i].tile_first = l + 1;
	}
}

/* Select the outermost bands in the elements of the list, align
 * their prefix schedules, separate bands with different values
 * for tile_len and/or n_parallel and then combine the resulting
 * prefix and suffix schedules into a single pair of prefix and
 * suffix schedules for the entire list.
 */
static void list_select_outer_band(struct cuda_gen *gen,
	__isl_take isl_band_list *list, int pos, struct band_info *list_info)
{
	isl_band *band;
	int i;
	int n = isl_band_list_n_band(list);
	isl_ctx *ctx = isl_band_list_get_ctx(list);
	struct band_info *info;
	int max_tile_first;
	isl_union_map *prefix;
	isl_union_map *suffix;

	assert(n >= 1);
	info = isl_calloc_array(ctx, struct band_info, n);
	assert(info);

	max_tile_first = 0;
	for (i = 0; i < n; ++i) {
		band = isl_band_list_get_band(list, i);
		band_select_outer_band(gen, band, pos, &info[i]);
		if (info[i].tile_first > max_tile_first)
			max_tile_first = info[i].tile_first;
	}

	for (i = 0; i < n; ++i) {
		if (info[i].tile_first == max_tile_first)
			continue;
		info[i].prefix = extend_range(info[i].prefix,
					info[i].tile_first, max_tile_first, 0);
	}

	qsort(info, n, sizeof(struct band_info), &cmp_band);

	for (i = 0; i < n - 1; ++i)
		if (info[i].tile_len != info[i + 1].tile_len ||
		    info[i].n_parallel != info[i + 1].n_parallel)
			break;

	if (i < n -1)
		separate_bands(info, n);

	prefix = info[0].prefix;
	suffix = info[0].suffix;

	for (i = 1; i < n; ++i) {
		prefix = isl_union_map_union(prefix, info[i].prefix);
		suffix = isl_union_map_union(suffix, info[i].suffix);
	}

	list_info->tile_first = info[0].tile_first;
	list_info->tile_len = -1;
	list_info->prefix = prefix;
	list_info->suffix = suffix;

	isl_band_list_free(list);
	free(info);
}

/* Set max_out to the maximal number of output dimensions over
 * all maps.
 */
static int update_max_out(__isl_take isl_map *map, void *user)
{
	int *max_out = user;
	int n_out = isl_map_dim(map, isl_dim_out);

	if (n_out > *max_out)
		*max_out = n_out;

	isl_map_free(map);
	return 0;
}

struct align_range_data {
	int max_out;
	isl_union_map *res;
};

/* Extend the dimension of the range of the given map to data->max_out and
 * then add the result to data->res.
 */
static int map_align_range(__isl_take isl_map *map, void *user)
{
	struct align_range_data *data = user;
	int i;
	isl_dim *dim;
	isl_map *proj;
	int n_out = isl_map_dim(map, isl_dim_out);

	dim = isl_union_map_get_dim(data->res);
	proj = isl_map_reverse(projection(dim, data->max_out, n_out));
	for (i = n_out; i < data->max_out; ++i)
		proj = isl_map_fix_si(proj, isl_dim_out, i, 0);

	map = isl_map_apply_range(map, proj);

	data->res = isl_union_map_add_map(data->res, map);

	return 0;
}

/* Extend the ranges of the maps in the union map such they all have
 * the same dimension.
 */
static __isl_give isl_union_map *align_range(__isl_take isl_union_map *umap)
{
	struct align_range_data data;

	data.max_out = 0;
	isl_union_map_foreach_map(umap, &update_max_out, &data.max_out);

	data.res = isl_union_map_empty(isl_union_map_get_dim(umap));
	isl_union_map_foreach_map(umap, &map_align_range, &data);

	isl_union_map_free(umap);
	return data.res;
}

/* Select the outermost tilable band that (by construction)
 * has at least one parallel loop.
 * The starting position of the aligned band is stored in the pair
 * gen->tile_first.
 * The sizes and number of parallel loops may be different in different
 * parts of the band forest and are therefore stored in the cuda_stmts.
 *
 * Return the complete schedule, with the tilable bands aligned
 * at gen->tile_first and padded with zero, if needed.
 */
static __isl_give isl_union_map *select_outer_tilable_band(struct cuda_gen *gen,
	__isl_keep isl_schedule *schedule)
{
	isl_band_list *list;
	struct band_info info;

	gen->n_parallel = 0;
	gen->tile_len = -1;

	list = isl_schedule_get_band_forest(schedule);

	list_select_outer_band(gen, list, 0, &info);

	gen->tile_first = info.tile_first;
	info.suffix = align_range(info.suffix);

	return isl_union_map_flat_range_product(info.prefix, info.suffix);
}

/* Set gen->untiled_len to the number of scheduling dimensions
 * for the schedule of the first domain.
 * We assume here that this number is the same for all domains.
 */
static int set_untiled_len(__isl_take isl_map *map, void *user)
{
	unsigned *untiled_len = user;

	*untiled_len = isl_map_dim(map, isl_dim_out);

	isl_map_free(map);
	return -1;
}

/* Compute an appropriate schedule based on the accesses in
 * gen->read and gen->write.
 *
 * We first compute dependences and then use those to compute
 * a schedule that has a parallel loop in each tilable band.
 * Finally, we select the outermost tilable band.
 */
static void compute_schedule(struct cuda_gen *gen,
	__isl_take isl_union_map *sched)
{
	isl_ctx *ctx = isl_union_map_get_ctx(sched);
	isl_union_set *domain;
	isl_union_map *empty;
	isl_union_map *dep_raw, *dep2, *dep3, *dep;
	isl_union_map *uninitialized;
	isl_schedule *schedule;
	struct isl_options *options;

	empty = isl_union_map_empty(isl_union_map_get_dim(sched));

        isl_union_map_compute_flow(isl_union_map_copy(gen->read),
                            isl_union_map_copy(gen->write), empty,
                            isl_union_map_copy(sched),
                            &dep_raw, NULL, &uninitialized, NULL);
        isl_union_map_compute_flow(isl_union_map_copy(gen->write),
                            isl_union_map_copy(gen->write),
                            isl_union_map_copy(gen->read),
                            isl_union_map_copy(sched),
                            &dep2, &dep3, NULL, NULL);
	isl_union_map_free(sched);

	gen->copy_in = isl_union_map_range(uninitialized);

	dep = isl_union_map_union(dep2, dep3);
	dep = isl_union_map_union(dep, dep_raw);
	dep = isl_union_map_coalesce(dep);

	domain = extract_domain(gen);
	options = isl_ctx_peek_options(ctx, isl_options_arg);
	options->schedule_outer_zero_distance = 1;
	schedule = isl_union_set_compute_schedule(isl_union_set_copy(domain),
				isl_union_map_copy(dep), dep);

	sched = select_outer_tilable_band(gen, schedule);

	isl_union_map_foreach_map(sched, &set_untiled_len, &gen->untiled_len);
	sched = isl_union_map_intersect_domain(sched, domain);
	gen->sched = sched;

	isl_schedule_free(schedule);
}

/* Replace the scop in the "input" file by equivalent code
 * that uses the GPU.  "scop" is assumed to correspond to this scop.
 *
 * We first compute a schedule that respects the dependences
 * of the original program and select the outermost band
 * of tilable dimensions that has at least one parallel loop.
 * We then have three blocks of dimensions
 *
 *	H		B			G
 *
 * The tilable band "B" is first tiled according to "tile.sizes", resulting
 * in
 *
 *	H	T		P		G
 *
 * For each iteration of the T loop and for each array, we compute
 * the array elements accessed by that iteration, construct a rectangular
 * box around it and shift it to the origin.  The result is used
 * as shared memory for the array.
 *
 * We then split off at most 2 parallel loops from the T loops and
 * at most 3 parallel loops from the P loops
 *
 *	H	T1	T2	P1	P2	G
 *
 * The T1/P1 loops are then tiled or "wrapped" over the blocks/threads,
 * according to "grid.sizes"/"block.sizes".
 *
 *	H	T1T T1P	T2	P1T P1P	P2	G
 *
 * Finally, the T1P and P1P iterators are equated to the block and
 * thread dimensions respectively and so are effectively removed.
 * The H loops are run on the host.  The T1T, T2, P1T, P2 and G loops
 * are run on the GPU.
 *
 * Code is generated in three stages.  We first generate code for the
 * host (the H loops), with iterators h%d.  Then, for each leaf node
 * of the resulting AST, we generate code for the shared loops (up to
 * and including T2), with iterators g%d and after equating the H loops
 * to h%d parameters and the T1P loops to the block dimensions.
 * Finally, we generate code for the remaining loops in a similar fashion.
 *
 * The function frees "scop" and "ctx".
 */
int cuda_scop(isl_ctx *ctx, scoplib_scop_p scop, struct ppcg_options *options,
	const char *input)
{
	isl_union_map *sched;
	struct cuda_gen gen;

	gen.ctx = ctx;
	gen.context = extract_context(gen.ctx, scop);
	gen.context = add_context_from_str(gen.context, options->ctx);
	gen.n_stmts = scoplib_statement_number(scop->statement);
	gen.stmts = extract_stmts(gen.ctx, scop, gen.context);
	extract_accesses(&gen, scop);
	gen.options = options;
	gen.state = cloog_isl_state_malloc(gen.ctx);

	cuda_open_files(&gen.cuda, input);

	collect_array_info(&gen);

	sched = extract_original_schedule(&gen, scop);
	compute_schedule(&gen, sched);

	print_host_code(&gen);

	cloog_state_free(gen.state);
	clear_cuda_gen(&gen);
	isl_ctx_free(gen.ctx);
	scoplib_scop_free(scop);

	cuda_close_files(&gen.cuda);

	return 0;
}