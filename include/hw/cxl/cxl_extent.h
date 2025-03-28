#ifndef CXL_EXTENT_H_H
#define CXL_EXTENT_H_H
#include<stdint.h>
#include<stdbool.h>
#include "hw/cxl/cxl_device.h"

#define MAX_PENDING_GROUPS(ct3d)(sizeof(ct3d->dc.shared_info->pending_groups) /\
                                 sizeof(ct3d->dc.shared_info->pending_groups[0]))

static inline int dc_extent_idx(CXLType3Dev *ct3d, CXLDCExtent *ent)
{
  if (!ent) {
    return -1;
  }
  return (ent - &ct3d->dc.shared_info->extent_pool[0]);
}

static inline CXLDCExtent *dc_extent_ptr(const CXLType3Dev *ct3d, int idx)
{
  if (idx == -1) {
    return NULL;
  }
  return &ct3d->dc.shared_info->extent_pool[idx];
}
/*
 * NOTE: during the iteration, the list should be immutable
 * The caller needs to check whether it iterates to the start again or not
 */
#define EXTENTLIST_FOREACH(ent, next, list, ct3d)                              \
  for (ent = dc_extent_ptr(ct3d, (list)->head);                                  \
       ent != NULL && (next = dc_extent_ptr(ct3d, ent->next), 1); ent = next)

#define EXTENTLIST_FIRST(ct3d, list)                                           \
  ((list)->head == -1 ? NULL : &ct3d->dc.shared_info->extent_pool[(list)->head])

static inline CXLDCExtentGroup *alloc_dc_extent_group(CXLType3Dev *ct3d) {
    int num = ct3d->dc.shared_info->num_pending_groups;

    if (num >= MAX_PENDING_GROUPS(ct3d)) {
        return NULL;
    }
    ct3d->dc.shared_info->num_pending_groups++;
    return &ct3d->dc.shared_info->pending_groups[num];
       }

/* Allocate one extent from the extent pool */
static int alloc_dc_extent(CXLType3Dev *ct3d)
{
    int idx = find_next_zero_bit(ct3d->dc.shared_info->extent_pool_bm,
                                 CXL_NUM_EXTENTS_SUPPORTED, 0);
    if (idx < CXL_NUM_EXTENTS_SUPPORTED) {
        bitmap_set(ct3d->dc.shared_info->extent_pool_bm, idx, 1);
        return idx;
    }
    return -1;
}

static void free_dc_extent(CXLType3Dev *ct3d, int idx)
{
    CXLDCExtent *ent;

    if (idx < 0 || idx > CXL_NUM_EXTENTS_SUPPORTED) {
        return;
    }
    ent = dc_extent_ptr(ct3d, idx);
    memset(ent, 0, sizeof(*ent));
    ent->next = -1;
    bitmap_clear(ct3d->dc.shared_info->extent_pool_bm, idx, 1);
}

static void cxl_insert_extent_to_extent_list(CXLType3Dev *ct3d,
                                             CXLDCExtentList *list,
                                             uint64_t dpa, uint64_t len,
                                             uint8_t *tag,
                                             uint16_t shared_seq)
{
    CXLDCExtent *extent, *p;
    int idx = alloc_dc_extent(ct3d);

    extent = &ct3d->dc.shared_info->extent_pool[idx];
    extent->start_dpa = dpa;
    extent->len = len;
    if (tag) {
        memcpy(extent->tag, tag, 0x10);
    }
    extent->shared_seq = shared_seq;
    extent->next = -1;

    if (list->head == -1) {
        list->head = idx;
    } else {
        p = dc_extent_ptr(ct3d, list->tail);
        p->next = idx;
    }
    list->tail = idx;
}

static void __attribute__((unused))
cxl_remove_extent_from_extent_list(CXLType3Dev *ct3d, CXLDCExtentList *list,
                                   CXLDCExtent *extent)
{
    CXLDCExtent *prev = NULL, *next = NULL, *ent;
    int idx = dc_extent_idx(ct3d, extent);

    EXTENTLIST_FOREACH(ent, next, list, ct3d) {
        if (ent == extent) {
          break;
        }
        prev = ent;
    }

    if (prev) {
        prev->next = extent->next;
    } else {
        list->head = extent->next;
    }

    if (idx == list->tail) {
        list->tail = dc_extent_idx(ct3d, prev);
    }
    free_dc_extent(ct3d, idx);
}

/*
 * Note: group should never be NULL, for empty group, group->head = -1
 * Add a new extent to the extent "group" if group exists;
 * otherwise, create a new group
 * Return value: the extent group where the extent is inserted.
 */
static inline CXLDCExtentGroup *
cxl_insert_extent_to_extent_group(CXLType3Dev *ct3d, CXLDCExtentGroup *group,
                                  uint64_t dpa, uint64_t len, uint8_t *tag,
                                  uint16_t shared_seq) {
    cxl_insert_extent_to_extent_list(ct3d, &group->list, dpa, len,
                                     tag, shared_seq);
    return group;
}

static int __attribute__((unused))
cxl_extent_group_list_insert_tail(CXLType3Dev *ct3d, CXLDCExtentGroup *group) {
    uint32_t num_pending  = ct3d->dc.shared_info->num_pending_groups;
    CXLDCExtentGroup *groupList = ct3d->dc.shared_info->pending_groups;

    if (num_pending >= sizeof(*groupList))
        return -1;

    groupList[num_pending] = *group;
    ct3d->dc.shared_info->num_pending_groups++;
    return 0;
}

/*
 * Check whether the range [dpa, dpa + len - 1] has overlaps with extents in
 * the list.
 * Return value: return true if has overlaps; otherwise, return false
 */
static bool __attribute__((unused))
cxl_extents_overlaps_dpa_range(CXLType3Dev *ct3d, CXLDCExtentList *list,
                               uint64_t dpa, uint64_t len)
{
    CXLDCExtent *ent, *next = NULL;
    Range range1, range2;

    if (!list) {
        return false;
    }

    range_init_nofail(&range1, dpa, len);
    EXTENTLIST_FOREACH(ent, next, list, ct3d) {
        range_init_nofail(&range2, ent->start_dpa, ent->len);
        if (range_overlaps_range(&range1, &range2)) {
            return true;
        }
    }
    return false;
}

/*
 * Check whether the range [dpa, dpa + len - 1] is contained by extents in
 * the list.
 * Will check multiple extents containment once superset release is added.
 * Return value: return true if range is contained; otherwise, return false
 */
static bool __attribute__((unused))
cxl_extents_contains_dpa_range(CXLType3Dev *ct3d, CXLDCExtentList *list,
                               uint64_t dpa, uint64_t len)
{
    CXLDCExtent *ent, *next;
    Range range1, range2;

    if (!list) {
        return false;
    }

    range_init_nofail(&range1, dpa, len);
    EXTENTLIST_FOREACH(ent, next, list, ct3d) {
        range_init_nofail(&range2, ent->start_dpa, ent->len);
        if (range_contains_range(&range2, &range1)) {
            return true;
        }
    }
    return false;
}

static bool __attribute__((unused))
cxl_extent_groups_overlaps_dpa_range(CXLType3Dev *ct3d, uint64_t dpa,
                                     uint64_t len)
{
    CXLDCExtentGroup *group;
    int i;

    for (i = 0; i < ct3d->dc.shared_info->num_pending_groups; i++) {
        group = &ct3d->dc.shared_info->pending_groups[i];
        if (cxl_extents_overlaps_dpa_range(ct3d, &group->list, dpa, len)) {
            return true;
        }
    }
    return false;
}

/*
 * Pending extent list always processed in order, so the ordering
 * of list remaining after the deletion should be kept.
 * */
static void __attribute__((unused))
cxl_extent_group_list_delete_front(CXLType3Dev *ct3d)
{
    uint32_t num_pending  = ct3d->dc.shared_info->num_pending_groups;
    CXLDCExtentGroup *group = &ct3d->dc.shared_info->pending_groups[0];
    CXLDCExtent *ent;
    int i;

    if (num_pending == 0) {
        return;
    }

    while ((ent = EXTENTLIST_FIRST(ct3d, &group->list))) {
        cxl_remove_extent_from_extent_list(ct3d, &group->list, ent);
    }

    for (i = 1; i < num_pending; i++) {
        group[i-1] = group[i];
    }
    group[num_pending - 1].list.head = -1;
    group[num_pending - 1].list.tail = -1;
    ct3d->dc.shared_info->num_pending_groups --;
}

static inline bool empty_dc_extent_group(CXLDCExtentGroup *group)
{
    return group->list.head == -1;
}

static inline void free_dc_extent_group(CXLType3Dev *ct3d, CXLDCExtentGroup *group)
{
    group->list.head = group->list.tail = -1;
    ct3d->dc.shared_info->num_pending_groups--;
}
#endif
