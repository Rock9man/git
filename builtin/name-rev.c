#include "builtin.h"
#include "cache.h"
#include "commit.h"
#include "tag.h"
#include "refs.h"
#include "parse-options.h"
#include "sha1-lookup.h"

#define CUTOFF_DATE_SLOP 86400 /* one day */

typedef struct rev_name {
	const char *tip_name;
	unsigned long taggerdate;
	int generation;
	int distance;
	int from_tag;
} rev_name;

static long cutoff = LONG_MAX;

static const char *prio_names[] = {
	N_("head"), N_("lightweight"), N_("annotated"),
};

/* How many generations are maximally preferred over _one_ merge traversal? */
#define MERGE_TRAVERSAL_WEIGHT 65535

static int is_better_name(struct rev_name *name,
			  const char *tip_name,
			  unsigned long taggerdate,
			  int generation,
			  int distance,
			  int from_tag)
{
	/*
	 * When comparing names based on tags, prefer names
	 * based on the older tag, even if it is farther away.
	 */
	if (from_tag && name->from_tag)
		return (name->taggerdate > taggerdate ||
			(name->taggerdate == taggerdate &&
			 name->distance > distance));

	/*
	 * We know that at least one of them is a non-tag at this point.
	 * favor a tag over a non-tag.
	 */
	if (name->from_tag != from_tag)
		return from_tag;

	/*
	 * We are now looking at two non-tags.  Tiebreak to favor
	 * shorter hops.
	 */
	if (name->distance != distance)
		return name->distance > distance;

	/* ... or tiebreak to favor older date */
	if (name->taggerdate != taggerdate)
		return name->taggerdate > taggerdate;

	/* keep the current one if we cannot decide */
	return 0;
}

struct name_ref_data {
	int tags_only;
	int name_only;
	int debug;
	struct string_list ref_filters;
	struct string_list exclude_filters;
	struct object_array *revs;
};

static void name_rev(struct commit *commit,
		const char *tip_name, unsigned long taggerdate,
		int generation, int distance, int from_tag,
		int deref, struct name_ref_data *data)
{
	struct rev_name *name = (struct rev_name *)commit->util;
	struct commit_list *parents;
	int parent_number = 1;

	parse_commit(commit);

	if (commit->date < cutoff)
		return;

	if (deref) {
		tip_name = xstrfmt("%s^0", tip_name);
		from_tag += 1;

		if (generation)
			die("generation: %d, but deref?", generation);
	}

	if (name == NULL) {
		name = xmalloc(sizeof(rev_name));
		commit->util = name;
		goto copy_data;
	} else if (is_better_name(name, tip_name, taggerdate,
				  generation, distance, from_tag)) {
copy_data:
		if (data->debug) {
			int i;
			static int label_width = -1;
			static int name_width = -1;
			if (label_width < 0) {
				int w;
				for (i = 0; i < ARRAY_SIZE(prio_names); i++) {
					w = strlen(_(prio_names[i]));
					if (label_width < w)
						label_width = w;
				}
			}
			if (name_width < 0) {
				int w;
				for (i = 0; i < data->revs->nr; i++) {
					w = strlen(data->revs->objects[i].name);
					if (name_width < w)
						name_width = w;
				}
			}
			for (i = 0; i < data->revs->nr; i++)
				if (!oidcmp(&commit->object.oid,
					    &data->revs->objects[i].item->oid))
					fprintf(stderr, " %-*s %8d %-*s %s~%d\n",
						label_width,
						_(prio_names[from_tag]),
						distance, name_width,
						data->revs->objects[i].name,
						tip_name, generation);
		}
		name->tip_name = tip_name;
		name->taggerdate = taggerdate;
		name->generation = generation;
		name->distance = distance;
		name->from_tag = from_tag;
	} else
		return;

	for (parents = commit->parents;
			parents;
			parents = parents->next, parent_number++) {
		if (parent_number > 1) {
			size_t len;
			char *new_name;

			strip_suffix(tip_name, "^0", &len);
			if (generation > 0)
				new_name = xstrfmt("%.*s~%d^%d", (int)len, tip_name,
						   generation, parent_number);
			else
				new_name = xstrfmt("%.*s^%d", (int)len, tip_name,
						   parent_number);

			name_rev(parents->item, new_name, taggerdate, 0,
				 distance + MERGE_TRAVERSAL_WEIGHT,
				 from_tag, 0, data);
		} else {
			name_rev(parents->item, tip_name, taggerdate,
				 generation + 1, distance + 1,
				 from_tag, 0, data);
		}
	}
}

static int subpath_matches(const char *path, const char *filter)
{
	const char *subpath = path;

	while (subpath) {
		if (!wildmatch(filter, subpath, 0, NULL))
			return subpath - path;
		subpath = strchr(subpath, '/');
		if (subpath)
			subpath++;
	}
	return -1;
}

static const char *name_ref_abbrev(const char *refname, int shorten_unambiguous)
{
	if (shorten_unambiguous)
		refname = shorten_unambiguous_ref(refname, 0);
	else if (starts_with(refname, "refs/heads/"))
		refname = refname + 11;
	else if (starts_with(refname, "refs/"))
		refname = refname + 5;
	return refname;
}

static struct tip_table {
	struct tip_table_entry {
		unsigned char sha1[20];
		const char *refname;
	} *table;
	int nr;
	int alloc;
	int sorted;
} tip_table;

static void add_to_tip_table(const unsigned char *sha1, const char *refname,
			     int shorten_unambiguous)
{
	refname = name_ref_abbrev(refname, shorten_unambiguous);

	ALLOC_GROW(tip_table.table, tip_table.nr + 1, tip_table.alloc);
	hashcpy(tip_table.table[tip_table.nr].sha1, sha1);
	tip_table.table[tip_table.nr].refname = xstrdup(refname);
	tip_table.nr++;
	tip_table.sorted = 0;
}

static int tipcmp(const void *a_, const void *b_)
{
	const struct tip_table_entry *a = a_, *b = b_;
	return hashcmp(a->sha1, b->sha1);
}

static int name_ref(const char *path, const struct object_id *oid, int flags, void *cb_data)
{
	struct object *o = parse_object(oid->hash);
	struct name_ref_data *data = cb_data;
	int can_abbreviate_output = data->tags_only && data->name_only;
	int deref = 0;
	unsigned long taggerdate = ULONG_MAX;

	if (data->tags_only && !starts_with(path, "refs/tags/"))
		return 0;

	if (data->exclude_filters.nr) {
		struct string_list_item *item;

		for_each_string_list_item(item, &data->exclude_filters) {
			if (subpath_matches(path, item->string) >= 0)
				return 0;
		}
	}

	if (data->ref_filters.nr) {
		struct string_list_item *item;
		int matched = 0;

		/* See if any of the patterns match. */
		for_each_string_list_item(item, &data->ref_filters) {
			/*
			 * Check all patterns even after finding a match, so
			 * that we can see if a match with a subpath exists.
			 * When a user asked for 'refs/tags/v*' and 'v1.*',
			 * both of which match, the user is showing her
			 * willingness to accept a shortened output by having
			 * the 'v1.*' in the acceptable refnames, so we
			 * shouldn't stop when seeing 'refs/tags/v1.4' matches
			 * 'refs/tags/v*'.  We should show it as 'v1.4'.
			 */
			switch (subpath_matches(path, item->string)) {
			case -1: /* did not match */
				break;
			case 0: /* matched fully */
				matched = 1;
				break;
			default: /* matched subpath */
				matched = 1;
				can_abbreviate_output = 1;
				break;
			}
		}

		/* If none of the patterns matched, stop now */
		if (!matched)
			return 0;
	}

	add_to_tip_table(oid->hash, path, can_abbreviate_output);
	while (o && o->type == OBJ_TAG) {
		struct tag *t = (struct tag *) o;
		if (!t->tagged)
			break; /* broken repository */
		o = parse_object(t->tagged->oid.hash);
		deref = 1;
		taggerdate = t->date;
	}
	if (o && o->type == OBJ_COMMIT) {
		struct commit *commit = (struct commit *)o;
		int from_tag = starts_with(path, "refs/tags/");

		if (taggerdate == ULONG_MAX)
			taggerdate = ((struct commit *)o)->date;
		path = name_ref_abbrev(path, can_abbreviate_output);
		name_rev(commit, xstrdup(path), taggerdate, 0, 0,
			 from_tag, deref, data);
	}
	return 0;
}

static const unsigned char *nth_tip_table_ent(size_t ix, void *table_)
{
	struct tip_table_entry *table = table_;
	return table[ix].sha1;
}

static const char *get_exact_ref_match(const struct object *o)
{
	int found;

	if (!tip_table.table || !tip_table.nr)
		return NULL;

	if (!tip_table.sorted) {
		QSORT(tip_table.table, tip_table.nr, tipcmp);
		tip_table.sorted = 1;
	}

	found = sha1_pos(o->oid.hash, tip_table.table, tip_table.nr,
			 nth_tip_table_ent);
	if (0 <= found)
		return tip_table.table[found].refname;
	return NULL;
}

/* returns a static buffer */
static const char *get_rev_name(const struct object *o)
{
	static char buffer[1024];
	struct rev_name *n;
	struct commit *c;

	if (o->type != OBJ_COMMIT)
		return get_exact_ref_match(o);
	c = (struct commit *) o;
	n = c->util;
	if (!n)
		return NULL;

	if (!n->generation)
		return n->tip_name;
	else {
		int len = strlen(n->tip_name);
		if (len > 2 && !strcmp(n->tip_name + len - 2, "^0"))
			len -= 2;
		snprintf(buffer, sizeof(buffer), "%.*s~%d", len, n->tip_name,
				n->generation);

		return buffer;
	}
}

static void show_name(const struct object *obj,
		      const char *caller_name,
		      int always, int allow_undefined, int name_only)
{
	const char *name;
	const struct object_id *oid = &obj->oid;

	if (!name_only)
		printf("%s ", caller_name ? caller_name : oid_to_hex(oid));
	name = get_rev_name(obj);
	if (name)
		printf("%s\n", name);
	else if (allow_undefined)
		printf("undefined\n");
	else if (always)
		printf("%s\n", find_unique_abbrev(oid->hash, DEFAULT_ABBREV));
	else
		die("cannot describe '%s'", oid_to_hex(oid));
}

static char const * const name_rev_usage[] = {
	N_("git name-rev [<options>] <commit>..."),
	N_("git name-rev [<options>] --all"),
	N_("git name-rev [<options>] --stdin"),
	NULL
};

static void name_rev_line(char *p, struct name_ref_data *data)
{
	int forty = 0;
	char *p_start;
	for (p_start = p; *p; p++) {
#define ishex(x) (isdigit((x)) || ((x) >= 'a' && (x) <= 'f'))
		if (!ishex(*p))
			forty = 0;
		else if (++forty == 40 &&
			 !ishex(*(p+1))) {
			unsigned char sha1[40];
			const char *name = NULL;
			char c = *(p+1);
			int p_len = p - p_start + 1;

			forty = 0;

			*(p+1) = 0;
			if (!get_sha1(p - 39, sha1)) {
				struct object *o =
					lookup_object(sha1);
				if (o)
					name = get_rev_name(o);
			}
			*(p+1) = c;

			if (!name)
				continue;

			if (data->name_only)
				printf("%.*s%s", p_len - 40, p_start, name);
			else
				printf("%.*s (%s)", p_len, p_start, name);
			p_start = p + 1;
		}
	}

	/* flush */
	if (p_start != p)
		fwrite(p_start, p - p_start, 1, stdout);
}

int cmd_name_rev(int argc, const char **argv, const char *prefix)
{
	struct object_array revs = OBJECT_ARRAY_INIT;
	int all = 0, transform_stdin = 0, allow_undefined = 1, always = 0, peel_tag = 0;
	struct name_ref_data data = { 0, 0, 0, STRING_LIST_INIT_NODUP, STRING_LIST_INIT_NODUP };
	struct option opts[] = {
		OPT_BOOL(0, "name-only", &data.name_only, N_("print only names (no SHA-1)")),
		OPT_BOOL(0, "tags", &data.tags_only, N_("only use tags to name the commits")),
		OPT_STRING_LIST(0, "refs", &data.ref_filters, N_("pattern"),
				   N_("only use refs matching <pattern>")),
		OPT_STRING_LIST(0, "exclude", &data.exclude_filters, N_("pattern"),
				   N_("ignore refs matching <pattern>")),
		OPT_GROUP(""),
		OPT_BOOL(0, "all", &all, N_("list all commits reachable from all refs")),
		OPT_BOOL(0, "debug", &data.debug, N_("debug search strategy on stderr")),
		OPT_BOOL(0, "stdin", &transform_stdin, N_("read from stdin")),
		OPT_BOOL(0, "undefined", &allow_undefined, N_("allow to print `undefined` names (default)")),
		OPT_BOOL(0, "always",     &always,
			   N_("show abbreviated commit object as fallback")),
		{
			/* A Hidden OPT_BOOL */
			OPTION_SET_INT, 0, "peel-tag", &peel_tag, NULL,
			N_("dereference tags in the input (internal use)"),
			PARSE_OPT_NOARG | PARSE_OPT_HIDDEN, NULL, 1,
		},
		OPT_END(),
	};

	git_config(git_default_config, NULL);
	argc = parse_options(argc, argv, prefix, opts, name_rev_usage, 0);
	if (all + transform_stdin + !!argc > 1) {
		error("Specify either a list, or --all, not both!");
		usage_with_options(name_rev_usage, opts);
	}
	if (all || transform_stdin)
		cutoff = 0;

	for (; argc; argc--, argv++) {
		unsigned char sha1[20];
		struct object *object;
		struct commit *commit;

		if (get_sha1(*argv, sha1)) {
			fprintf(stderr, "Could not get sha1 for %s. Skipping.\n",
					*argv);
			continue;
		}

		commit = NULL;
		object = parse_object(sha1);
		if (object) {
			struct object *peeled = deref_tag(object, *argv, 0);
			if (peeled && peeled->type == OBJ_COMMIT)
				commit = (struct commit *)peeled;
		}

		if (!object) {
			fprintf(stderr, "Could not get object for %s. Skipping.\n",
					*argv);
			continue;
		}

		if (commit) {
			if (cutoff > commit->date)
				cutoff = commit->date;
		}

		if (peel_tag) {
			if (!commit) {
				fprintf(stderr, "Could not get commit for %s. Skipping.\n",
					*argv);
				continue;
			}
			object = (struct object *)commit;
		}
		add_object_array(object, *argv, &revs);
	}

	if (cutoff)
		cutoff = cutoff - CUTOFF_DATE_SLOP;
	data.revs = &revs;
	for_each_ref(name_ref, &data);

	if (transform_stdin) {
		char buffer[2048];

		while (!feof(stdin)) {
			char *p = fgets(buffer, sizeof(buffer), stdin);
			if (!p)
				break;
			name_rev_line(p, &data);
		}
	} else if (all) {
		int i, max;

		max = get_max_object_index();
		for (i = 0; i < max; i++) {
			struct object *obj = get_indexed_object(i);
			if (!obj || obj->type != OBJ_COMMIT)
				continue;
			show_name(obj, NULL,
				  always, allow_undefined, data.name_only);
		}
	} else {
		int i;
		for (i = 0; i < revs.nr; i++)
			show_name(revs.objects[i].item, revs.objects[i].name,
				  always, allow_undefined, data.name_only);
	}

	return 0;
}
