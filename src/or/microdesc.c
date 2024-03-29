/* Copyright (c) 2009-2012, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#include "or.h"
#include "circuitbuild.h"
#include "config.h"
#include "directory.h"
#include "dirserv.h"
#include "microdesc.h"
#include "networkstatus.h"
#include "nodelist.h"
#include "policies.h"
#include "router.h"
#include "routerlist.h"
#include "routerparse.h"

/** A data structure to hold a bunch of cached microdescriptors.  There are
 * two active files in the cache: a "cache file" that we mmap, and a "journal
 * file" that we append to.  Periodically, we rebuild the cache file to hold
 * only the microdescriptors that we want to keep */
struct microdesc_cache_t {
  /** Map from sha256-digest to microdesc_t for every microdesc_t in the
   * cache. */
  HT_HEAD(microdesc_map, microdesc_t) map;

  /** Name of the cache file. */
  char *cache_fname;
  /** Name of the journal file. */
  char *journal_fname;
  /** Mmap'd contents of the cache file, or NULL if there is none. */
  tor_mmap_t *cache_content;
  /** Number of bytes used in the journal file. */
  size_t journal_len;
  /** Number of bytes in descriptors removed as too old. */
  size_t bytes_dropped;

  /** Total bytes of microdescriptor bodies we have added to this cache */
  uint64_t total_len_seen;
  /** Total number of microdescriptors we have added to this cache */
  unsigned n_seen;
};

/** Helper: computes a hash of <b>md</b> to place it in a hash table. */
static INLINE unsigned int
_microdesc_hash(microdesc_t *md)
{
  unsigned *d = (unsigned*)md->digest;
#if SIZEOF_INT == 4
  return d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[5] ^ d[6] ^ d[7];
#else
  return d[0] ^ d[1] ^ d[2] ^ d[3];
#endif
}

/** Helper: compares <b>a</b> and </b> for equality for hash-table purposes. */
static INLINE int
_microdesc_eq(microdesc_t *a, microdesc_t *b)
{
  return tor_memeq(a->digest, b->digest, DIGEST256_LEN);
}

HT_PROTOTYPE(microdesc_map, microdesc_t, node,
             _microdesc_hash, _microdesc_eq);
HT_GENERATE(microdesc_map, microdesc_t, node,
             _microdesc_hash, _microdesc_eq, 0.6,
             malloc, realloc, free);

/** Write the body of <b>md</b> into <b>f</b>, with appropriate annotations.
 * On success, return the total number of bytes written, and set
 * *<b>annotation_len_out</b> to the number of bytes written as
 * annotations. */
static ssize_t
dump_microdescriptor(FILE *f, microdesc_t *md, size_t *annotation_len_out)
{
  ssize_t r = 0;
  size_t written;
  /* XXXX drops unkown annotations. */
  if (md->last_listed) {
    char buf[ISO_TIME_LEN+1];
    char annotation[ISO_TIME_LEN+32];
    format_iso_time(buf, md->last_listed);
    tor_snprintf(annotation, sizeof(annotation), "@last-listed %s\n", buf);
    if (fputs(annotation, f) < 0) {
      log_warn(LD_DIR,
               "Couldn't write microdescriptor annotation: %s",
               strerror(ferror(f)));
      return -1;
    }
    r += strlen(annotation);
    *annotation_len_out = r;
  } else {
    *annotation_len_out = 0;
  }

  md->off = (off_t) ftell(f);
  written = fwrite(md->body, 1, md->bodylen, f);
  if (written != md->bodylen) {
    log_warn(LD_DIR,
             "Couldn't dump microdescriptor (wrote %lu out of %lu): %s",
             (unsigned long)written, (unsigned long)md->bodylen,
             strerror(ferror(f)));
    return -1;
  }
  r += md->bodylen;
  return r;
}

/** Holds a pointer to the current microdesc_cache_t object, or NULL if no
 * such object has been allocated. */
static microdesc_cache_t *the_microdesc_cache = NULL;

/** Return a pointer to the microdescriptor cache, loading it if necessary. */
microdesc_cache_t *
get_microdesc_cache(void)
{
  if (PREDICT_UNLIKELY(the_microdesc_cache==NULL)) {
    microdesc_cache_t *cache = tor_malloc_zero(sizeof(microdesc_cache_t));
    HT_INIT(microdesc_map, &cache->map);
    cache->cache_fname = get_datadir_fname("cached-microdescs");
    cache->journal_fname = get_datadir_fname("cached-microdescs.new");
    microdesc_cache_reload(cache);
    the_microdesc_cache = cache;
  }
  return the_microdesc_cache;
}

/* There are three sources of microdescriptors:
   1) Generated by us while acting as a directory authority.
   2) Loaded from the cache on disk.
   3) Downloaded.
*/

/** Decode the microdescriptors from the string starting at <b>s</b> and
 * ending at <b>eos</b>, and store them in <b>cache</b>.  If <b>no-save</b>,
 * mark them as non-writable to disk.  If <b>where</b> is SAVED_IN_CACHE,
 * leave their bodies as pointers to the mmap'd cache.  If where is
 * <b>SAVED_NOWHERE</b>, do not allow annotations.  If listed_at is positive,
 * set the last_listed field of every microdesc to listed_at.  If
 * requested_digests is non-null, then it contains a list of digests we mean
 * to allow, so we should reject any non-requested microdesc with a different
 * digest, and alter the list to contain only the digests of those microdescs
 * we didn't find.
 * Return a newly allocated list of the added microdescriptors, or NULL  */
smartlist_t *
microdescs_add_to_cache(microdesc_cache_t *cache,
                        const char *s, const char *eos, saved_location_t where,
                        int no_save, time_t listed_at,
                        smartlist_t *requested_digests256)
{
  smartlist_t *descriptors, *added;
  const int allow_annotations = (where != SAVED_NOWHERE);
  const int copy_body = (where != SAVED_IN_CACHE);

  descriptors = microdescs_parse_from_string(s, eos,
                                             allow_annotations,
                                             copy_body);
  if (listed_at > 0) {
    SMARTLIST_FOREACH(descriptors, microdesc_t *, md,
                      md->last_listed = listed_at);
  }
  if (requested_digests256) {
    digestmap_t *requested; /* XXXX actuqlly we should just use a
                               digest256map */
    requested = digestmap_new();
    SMARTLIST_FOREACH(requested_digests256, const char *, cp,
      digestmap_set(requested, cp, (void*)1));
    SMARTLIST_FOREACH_BEGIN(descriptors, microdesc_t *, md) {
      if (digestmap_get(requested, md->digest)) {
        digestmap_set(requested, md->digest, (void*)2);
      } else {
        log_fn(LOG_PROTOCOL_WARN, LD_DIR, "Received non-requested microcdesc");
        microdesc_free(md);
        SMARTLIST_DEL_CURRENT(descriptors, md);
      }
    } SMARTLIST_FOREACH_END(md);
    SMARTLIST_FOREACH_BEGIN(requested_digests256, char *, cp) {
      if (digestmap_get(requested, cp) == (void*)2) {
        tor_free(cp);
        SMARTLIST_DEL_CURRENT(requested_digests256, cp);
      }
    } SMARTLIST_FOREACH_END(cp);
    digestmap_free(requested, NULL);
  }

  added = microdescs_add_list_to_cache(cache, descriptors, where, no_save);
  smartlist_free(descriptors);
  return added;
}

/** As microdescs_add_to_cache, but takes a list of micrdescriptors instead of
 * a string to decode.  Frees any members of <b>descriptors</b> that it does
 * not add. */
smartlist_t *
microdescs_add_list_to_cache(microdesc_cache_t *cache,
                             smartlist_t *descriptors, saved_location_t where,
                             int no_save)
{
  smartlist_t *added;
  open_file_t *open_file = NULL;
  FILE *f = NULL;
  //  int n_added = 0;
  ssize_t size = 0;

  if (where == SAVED_NOWHERE && !no_save) {
    f = start_writing_to_stdio_file(cache->journal_fname,
                                    OPEN_FLAGS_APPEND|O_BINARY,
                                    0600, &open_file);
    if (!f) {
      log_warn(LD_DIR, "Couldn't append to journal in %s: %s",
               cache->journal_fname, strerror(errno));
      return NULL;
    }
  }

  added = smartlist_new();
  SMARTLIST_FOREACH_BEGIN(descriptors, microdesc_t *, md) {
    microdesc_t *md2;
    md2 = HT_FIND(microdesc_map, &cache->map, md);
    if (md2) {
      /* We already had this one. */
      if (md2->last_listed < md->last_listed)
        md2->last_listed = md->last_listed;
      microdesc_free(md);
      if (where != SAVED_NOWHERE)
        cache->bytes_dropped += size;
      continue;
    }

    /* Okay, it's a new one. */
    if (f) {
      size_t annotation_len;
      size = dump_microdescriptor(f, md, &annotation_len);
      if (size < 0) {
        /* we already warned in dump_microdescriptor; */
        abort_writing_to_file(open_file);
        smartlist_clear(added);
        return added;
      }
      md->saved_location = SAVED_IN_JOURNAL;
      cache->journal_len += size;
    } else {
      md->saved_location = where;
    }

    md->no_save = no_save;

    HT_INSERT(microdesc_map, &cache->map, md);
    md->held_in_map = 1;
    smartlist_add(added, md);
    ++cache->n_seen;
    cache->total_len_seen += md->bodylen;
  } SMARTLIST_FOREACH_END(md);

  if (f)
    finish_writing_to_file(open_file); /*XXX Check me.*/

  {
    networkstatus_t *ns = networkstatus_get_latest_consensus();
    if (ns && ns->flavor == FLAV_MICRODESC)
      SMARTLIST_FOREACH(added, microdesc_t *, md, nodelist_add_microdesc(md));
  }

  if (smartlist_len(added))
    router_dir_info_changed();

  return added;
}

/** Remove every microdescriptor in <b>cache</b>. */
void
microdesc_cache_clear(microdesc_cache_t *cache)
{
  microdesc_t **entry, **next;
  for (entry = HT_START(microdesc_map, &cache->map); entry; entry = next) {
    microdesc_t *md = *entry;
    next = HT_NEXT_RMV(microdesc_map, &cache->map, entry);
    md->held_in_map = 0;
    microdesc_free(md);
  }
  HT_CLEAR(microdesc_map, &cache->map);
  if (cache->cache_content) {
    tor_munmap_file(cache->cache_content);
    cache->cache_content = NULL;
  }
  cache->total_len_seen = 0;
  cache->n_seen = 0;
  cache->bytes_dropped = 0;
}

/** Reload the contents of <b>cache</b> from disk.  If it is empty, load it
 * for the first time.  Return 0 on success, -1 on failure. */
int
microdesc_cache_reload(microdesc_cache_t *cache)
{
  struct stat st;
  char *journal_content;
  smartlist_t *added;
  tor_mmap_t *mm;
  int total = 0;

  microdesc_cache_clear(cache);

  mm = cache->cache_content = tor_mmap_file(cache->cache_fname);
  if (mm) {
    added = microdescs_add_to_cache(cache, mm->data, mm->data+mm->size,
                                    SAVED_IN_CACHE, 0, -1, NULL);
    if (added) {
      total += smartlist_len(added);
      smartlist_free(added);
    }
  }

  journal_content = read_file_to_str(cache->journal_fname,
                                     RFTS_IGNORE_MISSING, &st);
  if (journal_content) {
    cache->journal_len = (size_t) st.st_size;
    added = microdescs_add_to_cache(cache, journal_content,
                                    journal_content+st.st_size,
                                    SAVED_IN_JOURNAL, 0, -1, NULL);
    if (added) {
      total += smartlist_len(added);
      smartlist_free(added);
    }
    tor_free(journal_content);
  }
  log_notice(LD_DIR, "Reloaded microdescriptor cache.  Found %d descriptors.",
             total);

  microdesc_cache_rebuild(cache, 0 /* don't force */);

  return 0;
}

/** By default, we remove any microdescriptors that have gone at least this
 * long without appearing in a current consensus. */
#define TOLERATE_MICRODESC_AGE (7*24*60*60)

/** Remove all microdescriptors from <b>cache</b> that haven't been listed for
 * a long time.  Does not rebuild the cache on disk.  If <b>cutoff</b> is
 * positive, specifically remove microdescriptors that have been unlisted
 * since <b>cutoff</b>.  If <b>force</b> is true, remove microdescriptors even
 * if we have no current live microdescriptor consensus.
 */
void
microdesc_cache_clean(microdesc_cache_t *cache, time_t cutoff, int force)
{
  microdesc_t **mdp, *victim;
  int dropped=0, kept=0;
  size_t bytes_dropped = 0;
  time_t now = time(NULL);

  /* If we don't know a live consensus, don't believe last_listed values: we
   * might be starting up after being down for a while. */
  if (! force &&
      ! networkstatus_get_reasonably_live_consensus(now, FLAV_MICRODESC))
      return;

  if (cutoff <= 0)
    cutoff = now - TOLERATE_MICRODESC_AGE;

  for (mdp = HT_START(microdesc_map, &cache->map); mdp != NULL; ) {
    if ((*mdp)->last_listed < cutoff) {
      ++dropped;
      victim = *mdp;
      mdp = HT_NEXT_RMV(microdesc_map, &cache->map, mdp);
      victim->held_in_map = 0;
      bytes_dropped += victim->bodylen;
      microdesc_free(victim);
    } else {
      ++kept;
      mdp = HT_NEXT(microdesc_map, &cache->map, mdp);
    }
  }

  if (dropped) {
    log_notice(LD_DIR, "Removed %d/%d microdescriptors as old.",
               dropped,dropped+kept);
    cache->bytes_dropped += bytes_dropped;
  }
}

static int
should_rebuild_md_cache(microdesc_cache_t *cache)
{
    const size_t old_len =
      cache->cache_content ? cache->cache_content->size : 0;
    const size_t journal_len = cache->journal_len;
    const size_t dropped = cache->bytes_dropped;

    if (journal_len < 16384)
      return 0; /* Don't bother, not enough has happened yet. */
    if (dropped > (journal_len + old_len) / 3)
      return 1; /* We could save 1/3 or more of the currently used space. */
    if (journal_len > old_len / 2)
      return 1; /* We should append to the regular file */

    return 0;
}

/** Regenerate the main cache file for <b>cache</b>, clear the journal file,
 * and update every microdesc_t in the cache with pointers to its new
 * location.  If <b>force</b> is true, do this unconditionally.  If
 * <b>force</b> is false, do it only if we expect to save space on disk. */
int
microdesc_cache_rebuild(microdesc_cache_t *cache, int force)
{
  open_file_t *open_file;
  FILE *f;
  microdesc_t **mdp;
  smartlist_t *wrote;
  ssize_t size;
  off_t off = 0;
  int orig_size, new_size;

  if (cache == NULL) {
    cache = the_microdesc_cache;
    if (cache == NULL)
      return 0;
  }

  /* Remove dead descriptors */
  microdesc_cache_clean(cache, 0/*cutoff*/, 0/*force*/);

  if (!force && !should_rebuild_md_cache(cache))
    return 0;

  log_info(LD_DIR, "Rebuilding the microdescriptor cache...");

  orig_size = (int)(cache->cache_content ? cache->cache_content->size : 0);
  orig_size += (int)cache->journal_len;

  f = start_writing_to_stdio_file(cache->cache_fname,
                                  OPEN_FLAGS_REPLACE|O_BINARY,
                                  0600, &open_file);
  if (!f)
    return -1;

  wrote = smartlist_new();

  HT_FOREACH(mdp, microdesc_map, &cache->map) {
    microdesc_t *md = *mdp;
    size_t annotation_len;
    if (md->no_save)
      continue;

    size = dump_microdescriptor(f, md, &annotation_len);
    if (size < 0) {
      /* XXX handle errors from dump_microdescriptor() */
      /* log?  return -1?  die?  coredump the universe? */
      continue;
    }
    tor_assert(((size_t)size) == annotation_len + md->bodylen);
    md->off = off + annotation_len;
    off += size;
    if (md->saved_location != SAVED_IN_CACHE) {
      tor_free(md->body);
      md->saved_location = SAVED_IN_CACHE;
    }
    smartlist_add(wrote, md);
  }

  if (cache->cache_content)
    tor_munmap_file(cache->cache_content);

  finish_writing_to_file(open_file); /*XXX Check me.*/

  cache->cache_content = tor_mmap_file(cache->cache_fname);

  if (!cache->cache_content && smartlist_len(wrote)) {
    log_err(LD_DIR, "Couldn't map file that we just wrote to %s!",
            cache->cache_fname);
    smartlist_free(wrote);
    return -1;
  }
  SMARTLIST_FOREACH_BEGIN(wrote, microdesc_t *, md) {
    tor_assert(md->saved_location == SAVED_IN_CACHE);
    md->body = (char*)cache->cache_content->data + md->off;
    if (PREDICT_UNLIKELY(
             md->bodylen < 9 || fast_memneq(md->body, "onion-key", 9) != 0)) {
      /* XXXX once bug 2022 is solved, we can kill this block and turn it
       * into just the tor_assert(!memcmp) */
      off_t avail = cache->cache_content->size - md->off;
      char *bad_str;
      tor_assert(avail >= 0);
      bad_str = tor_strndup(md->body, MIN(128, (size_t)avail));
      log_err(LD_BUG, "After rebuilding microdesc cache, offsets seem wrong. "
              " At offset %d, I expected to find a microdescriptor starting "
              " with \"onion-key\".  Instead I got %s.",
              (int)md->off, escaped(bad_str));
      tor_free(bad_str);
      tor_assert(fast_memeq(md->body, "onion-key", 9));
    }
  } SMARTLIST_FOREACH_END(md);

  smartlist_free(wrote);

  write_str_to_file(cache->journal_fname, "", 1);
  cache->journal_len = 0;
  cache->bytes_dropped = 0;

  new_size = cache->cache_content ? (int)cache->cache_content->size : 0;
  log_info(LD_DIR, "Done rebuilding microdesc cache. "
           "Saved %d bytes; %d still used.",
           orig_size-new_size, new_size);

  return 0;
}

/** Make sure that the reference count of every microdescriptor in cache is
 * accurate. */
void
microdesc_check_counts(void)
{
  microdesc_t **mdp;
  if (!the_microdesc_cache)
    return;

  HT_FOREACH(mdp, microdesc_map, &the_microdesc_cache->map) {
    microdesc_t *md = *mdp;
    unsigned int found=0;
    const smartlist_t *nodes = nodelist_get_list();
    SMARTLIST_FOREACH(nodes, node_t *, node, {
        if (node->md == md) {
          ++found;
        }
      });
    tor_assert(found == md->held_by_nodes);
  }
}

/** Deallocate a single microdescriptor.  Note: the microdescriptor MUST have
 * previously been removed from the cache if it had ever been inserted. */
void
microdesc_free(microdesc_t *md)
{
  if (!md)
    return;

  /* Make sure that the microdesc was really removed from the appropriate data
     structures. */
  if (md->held_in_map) {
    microdesc_cache_t *cache = get_microdesc_cache();
    microdesc_t *md2 = HT_FIND(microdesc_map, &cache->map, md);
    if (md2 == md) {
      log_warn(LD_BUG, "microdesc_free() called, but md was still in "
               "microdesc_map");
      HT_REMOVE(microdesc_map, &cache->map, md);
    } else {
      log_warn(LD_BUG, "microdesc_free() called with held_in_map set, but "
               "microdesc was not in the map.");
    }
    tor_fragile_assert();
  }
  if (md->held_by_nodes) {
    int found=0;
    const smartlist_t *nodes = nodelist_get_list();
    SMARTLIST_FOREACH(nodes, node_t *, node, {
        if (node->md == md) {
          ++found;
          node->md = NULL;
        }
      });
    if (found) {
      log_warn(LD_BUG, "microdesc_free() called, but md was still referenced "
               "%d node(s); held_by_nodes == %u", found, md->held_by_nodes);
    } else {
      log_warn(LD_BUG, "microdesc_free() called with held_by_nodes set to %u, "
               "but md was not referenced by any nodes", md->held_by_nodes);
    }
    tor_fragile_assert();
  }
  //tor_assert(md->held_in_map == 0);
  //tor_assert(md->held_by_nodes == 0);

  if (md->onion_pkey)
    crypto_pk_free(md->onion_pkey);
  if (md->body && md->saved_location != SAVED_IN_CACHE)
    tor_free(md->body);

  if (md->family) {
    SMARTLIST_FOREACH(md->family, char *, cp, tor_free(cp));
    smartlist_free(md->family);
  }
  short_policy_free(md->exit_policy);

  tor_free(md);
}

/** Free all storage held in the microdesc.c module. */
void
microdesc_free_all(void)
{
  if (the_microdesc_cache) {
    microdesc_cache_clear(the_microdesc_cache);
    tor_free(the_microdesc_cache->cache_fname);
    tor_free(the_microdesc_cache->journal_fname);
    tor_free(the_microdesc_cache);
  }
}

/** If there is a microdescriptor in <b>cache</b> whose sha256 digest is
 * <b>d</b>, return it.  Otherwise return NULL. */
microdesc_t *
microdesc_cache_lookup_by_digest256(microdesc_cache_t *cache, const char *d)
{
  microdesc_t *md, search;
  if (!cache)
    cache = get_microdesc_cache();
  memcpy(search.digest, d, DIGEST256_LEN);
  md = HT_FIND(microdesc_map, &cache->map, &search);
  return md;
}

/** Return the mean size of decriptors added to <b>cache</b> since it was last
 * cleared.  Used to estimate the size of large downloads. */
size_t
microdesc_average_size(microdesc_cache_t *cache)
{
  if (!cache)
    cache = get_microdesc_cache();
  if (!cache->n_seen)
    return 512;
  return (size_t)(cache->total_len_seen / cache->n_seen);
}

/** Return a smartlist of all the sha256 digest of the microdescriptors that
 * are listed in <b>ns</b> but not present in <b>cache</b>. Returns pointers
 * to internals of <b>ns</b>; you should not free the members of the resulting
 * smartlist.  Omit all microdescriptors whose digest appear in <b>skip</b>. */
smartlist_t *
microdesc_list_missing_digest256(networkstatus_t *ns, microdesc_cache_t *cache,
                                 int downloadable_only, digestmap_t *skip)
{
  smartlist_t *result = smartlist_new();
  time_t now = time(NULL);
  tor_assert(ns->flavor == FLAV_MICRODESC);
  SMARTLIST_FOREACH_BEGIN(ns->routerstatus_list, routerstatus_t *, rs) {
    if (microdesc_cache_lookup_by_digest256(cache, rs->descriptor_digest))
      continue;
    if (downloadable_only &&
        !download_status_is_ready(&rs->dl_status, now,
                                  MAX_MICRODESC_DOWNLOAD_FAILURES))
      continue;
    if (skip && digestmap_get(skip, rs->descriptor_digest))
      continue;
    if (tor_mem_is_zero(rs->descriptor_digest, DIGEST256_LEN)) {
      log_info(LD_BUG, "Found an entry in networkstatus with no "
               "microdescriptor digest. (Router %s=%s at %s:%d.)",
               rs->nickname, hex_str(rs->identity_digest, DIGEST_LEN),
               fmt_addr32(rs->addr), rs->or_port);
      continue;
    }
    /* XXXX Also skip if we're a noncache and wouldn't use this router.
     * XXXX NM Microdesc
     */
    smartlist_add(result, rs->descriptor_digest);
  } SMARTLIST_FOREACH_END(rs);
  return result;
}

/** Launch download requests for mircodescriptors as appropriate.
 *
 * Specifically, we should launch download requests if we are configured to
 * download mirodescriptors, and there are some microdescriptors listed the
 * current microdesc consensus that we don't have, and either we never asked
 * for them, or we failed to download them but we're willing to retry.
 */
void
update_microdesc_downloads(time_t now)
{
  const or_options_t *options = get_options();
  networkstatus_t *consensus;
  smartlist_t *missing;
  digestmap_t *pending;

  if (should_delay_dir_fetches(options))
    return;
  if (directory_too_idle_to_fetch_descriptors(options, now))
    return;

  consensus = networkstatus_get_reasonably_live_consensus(now, FLAV_MICRODESC);
  if (!consensus)
    return;

  if (!we_fetch_microdescriptors(options))
    return;

  pending = digestmap_new();
  list_pending_microdesc_downloads(pending);

  missing = microdesc_list_missing_digest256(consensus,
                                             get_microdesc_cache(),
                                             1,
                                             pending);
  digestmap_free(pending, NULL);

  launch_descriptor_downloads(DIR_PURPOSE_FETCH_MICRODESC,
                              missing, NULL, now);

  smartlist_free(missing);
}

/** For every microdescriptor listed in the current microdecriptor consensus,
 * update its last_listed field to be at least as recent as the publication
 * time of the current microdescriptor consensus.
 */
void
update_microdescs_from_networkstatus(time_t now)
{
  microdesc_cache_t *cache = get_microdesc_cache();
  microdesc_t *md;
  networkstatus_t *ns =
    networkstatus_get_reasonably_live_consensus(now, FLAV_MICRODESC);

  if (! ns)
    return;

  tor_assert(ns->flavor == FLAV_MICRODESC);

  SMARTLIST_FOREACH_BEGIN(ns->routerstatus_list, routerstatus_t *, rs) {
    md = microdesc_cache_lookup_by_digest256(cache, rs->descriptor_digest);
    if (md && ns->valid_after > md->last_listed)
      md->last_listed = ns->valid_after;
  } SMARTLIST_FOREACH_END(rs);
}

/** Return true iff we should prefer to use microdescriptors rather than
 * routerdescs for building circuits. */
int
we_use_microdescriptors_for_circuits(const or_options_t *options)
{
  int ret = options->UseMicrodescriptors;
  if (ret == -1) {
    /* UseMicrodescriptors is "auto"; we need to decide: */
    /* If we are configured to use bridges and one of our bridges doesn't
     * know what a microdescriptor is, the answer is no. */
    if (options->UseBridges && any_bridges_dont_support_microdescriptors())
      return 0;
    /* Otherwise, we decide that we'll use microdescriptors iff we are
     * not a server, and we're not autofetching everything. */
    /* XXX023 what does not being a server have to do with it? also there's
     * a partitioning issue here where bridges differ from clients. */
    ret = !server_mode(options) && !options->FetchUselessDescriptors;
  }
  return ret;
}

/** Return true iff we should try to download microdescriptors at all. */
int
we_fetch_microdescriptors(const or_options_t *options)
{
  if (directory_caches_dir_info(options))
    return 1;
  if (options->FetchUselessDescriptors)
    return 1;
  return we_use_microdescriptors_for_circuits(options);
}

/** Return true iff we should try to download router descriptors at all. */
int
we_fetch_router_descriptors(const or_options_t *options)
{
  if (directory_caches_dir_info(options))
    return 1;
  if (options->FetchUselessDescriptors)
    return 1;
  return ! we_use_microdescriptors_for_circuits(options);
}

/** Return the consensus flavor we actually want to use to build circuits. */
int
usable_consensus_flavor(void)
{
  if (we_use_microdescriptors_for_circuits(get_options())) {
    return FLAV_MICRODESC;
  } else {
    return FLAV_NS;
  }
}

