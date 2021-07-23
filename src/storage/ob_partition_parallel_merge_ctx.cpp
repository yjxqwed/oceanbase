/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#include "storage/ob_partition_parallel_merge_ctx.h"
#include "storage/ob_partition_merge_task.h"
#include "storage/memtable/ob_memtable.h"
#include "storage/blocksstable/ob_macro_block_meta_mgr.h"
#include "share/config/ob_server_config.h"
#include "observer/omt/ob_tenant_config_mgr.h"
#include "ob_partition_range_spliter.h"
namespace oceanbase {
using namespace common;
using namespace share::schema;
using namespace share;
using namespace blocksstable;

namespace storage {

ObParallelMergeCtx::ObParallelMergeCtx()
    : parallel_type_(INVALID_PARALLEL_TYPE),
      range_array_(),
      first_sstable_(nullptr),
      concurrent_cnt_(0),
      allocator_(ObModIds::OB_CS_MERGER, OB_MALLOC_NORMAL_BLOCK_SIZE),
      is_inited_(false),
      major_merge_range_split_use_new_way_(false)
{}

ObParallelMergeCtx::~ObParallelMergeCtx()
{
  reset();
}

void ObParallelMergeCtx::reset()
{
  parallel_type_ = INVALID_PARALLEL_TYPE;
  range_array_.reset();
  first_sstable_ = nullptr;
  concurrent_cnt_ = 0;
  allocator_.reset();
  is_inited_ = false;
}

bool ObParallelMergeCtx::is_valid() const
{
  bool bret = true;
  if (IS_NOT_INIT || concurrent_cnt_ <= 0 || parallel_type_ >= INVALID_PARALLEL_TYPE) {
    bret = false;
  } else if (PARALLEL_MAJOR == parallel_type_) {
    // PARALLEL_MAJOR
    bret = OB_NOT_NULL(first_sstable_);
  } else if (range_array_.count() != concurrent_cnt_) {
    bret = false;
  } else if (concurrent_cnt_ > 1 && SERIALIZE_MERGE == parallel_type_) {
    bret = false;
  }
  return bret;
}

int ObParallelMergeCtx::init(ObSSTableMergeCtx& merge_ctx)
{
  int ret = OB_SUCCESS;

  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    STORAGE_LOG(WARN, "ObParallelMergeCtx init twice", K(ret));
  } else if (OB_UNLIKELY(nullptr == merge_ctx.table_schema_ || merge_ctx.tables_handle_.empty())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to init parallel merge", K(ret), K(merge_ctx));
  } else {
    int64_t tablet_size = merge_ctx.table_schema_->get_tablet_size();
    bool enable_parallel_minor_merge = false;
    omt::ObTenantConfigGuard tenant_config(TENANT_CONF(merge_ctx.table_schema_->get_tenant_id()));
    if (tenant_config.is_valid()) {
      enable_parallel_minor_merge = tenant_config->_enable_parallel_minor_merge;
    }
    if (enable_parallel_minor_merge && tablet_size > 0 && merge_ctx.param_.is_mini_merge()) {
      // TODO use memtable to decide parallel degree
      if (OB_FAIL(init_parallel_mini_merge(merge_ctx))) {
        STORAGE_LOG(WARN, "Failed to init parallel setting for mini merge", K(ret));
      }
      // TODO  support parallel buffer minor merge in future
    } else if (enable_parallel_minor_merge && tablet_size > 0 && merge_ctx.param_.is_minor_merge()) {
      if (OB_FAIL(init_parallel_mini_minor_merge(merge_ctx))) {
        STORAGE_LOG(WARN, "Failed to init parallel setting for mini minor merge", K(ret));
      }
    } else if (tablet_size >= 0 && merge_ctx.param_.is_major_merge()) {
      if (OB_FAIL(init_parallel_major_merge(merge_ctx))) {
        STORAGE_LOG(WARN, "Failed to init parallel major merge", K(ret));
      }
    } else if (OB_FAIL(init_serial_merge())) {
      STORAGE_LOG(WARN, "Failed to init serialize merge", K(ret));
    }
    if (OB_SUCC(ret)) {
      is_inited_ = true;
      STORAGE_LOG(
          INFO, "Succ to init parallel merge ctx", K(enable_parallel_minor_merge), K(tablet_size), K(merge_ctx.param_));
    }
  }

  return ret;
}

int ObParallelMergeCtx::get_merge_range(
    const int64_t parallel_idx, ObExtStoreRange& merge_range, ObIAllocator& allocator)
{
  int ret = OB_SUCCESS;

  if (!is_valid()) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "ObParallelMergeCtx is not inited", K(ret), K(*this));
  } else if (parallel_idx >= concurrent_cnt_) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to get parallel mergerange", K(ret), K(parallel_idx), K_(concurrent_cnt));
  } else {
    switch (parallel_type_) {
      case PARALLEL_MAJOR:
        if (major_merge_range_split_use_new_way_) {
          merge_range = range_array_.at(parallel_idx);
        } else if (OB_FAIL(first_sstable_->get_range(parallel_idx, concurrent_cnt_, allocator, merge_range))) {
          STORAGE_LOG(WARN, "Failed to get merge_range from the first sstable", K(ret));
        }
        break;
      case PARALLEL_MINI:
      case PARALLEL_MINI_MINOR:
      case SERIALIZE_MERGE:
        merge_range = range_array_.at(parallel_idx);
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(ERROR, "Unexpected parallel merge type", K(ret), K(*this));
        break;
    }
  }

  return ret;
}

int ObParallelMergeCtx::init_serial_merge()
{
  int ret = OB_SUCCESS;
  ObExtStoreRange merge_range;
  merge_range.get_range().set_whole_range();
  range_array_.reset();
  if (OB_FAIL(merge_range.to_collation_free_range_on_demand_and_cutoff_range(allocator_))) {
    STORAGE_LOG(WARN, "Failed to transform and cut off range", K(ret));
  } else if (OB_FAIL(range_array_.push_back(merge_range))) {
    STORAGE_LOG(WARN, "Failed to push back merge range to array", K(ret), K(merge_range));
  } else {
    concurrent_cnt_ = 1;
    parallel_type_ = SERIALIZE_MERGE;
  }

  return ret;
}

// to check whether the ranges are contiguous (valid)
static int check_contiguous_ranges(const common::ObIArray<common::ObStoreRange>& ranges)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(ranges.count()) < 1) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "Unexpected bad ranges (empty)", K(ret), K(ranges));
  } else if (OB_UNLIKELY(!ranges.at(0).get_start_key().is_min())) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "Unexpected bad ranges (first start key not min)", K(ret), K(ranges));
  } else if (OB_UNLIKELY(!ranges.at(ranges.count() - 1).get_end_key().is_max())) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "Unexpected bad ranges (last end key not max)", K(ret), K(ranges));
  } else {
    int cmp = 0;
    for (int64_t i = 1; i < ranges.count(); i++) {
      if (OB_FAIL(ranges.at(i).get_start_key().compare(ranges.at(i - 1).get_end_key(), cmp))) {
        STORAGE_LOG(WARN, "Failed to compare rowkeys", K(ret), K(i), K(ranges));
      } else if (OB_UNLIKELY(cmp != 0)) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "Unexpected bad ranges (not contiguous)", K(ret), K(i), K(ranges));
        break;
      }
    }
  }

  return ret;
}

int ObParallelMergeCtx::get_ranges_by_base_sstable(common::ObIArray<common::ObStoreRange>& ranges)
{
  int ret = OB_SUCCESS;

  const common::ObIArray<blocksstable::MacroBlockId>& macro_block_ids = first_sstable_->get_macro_block_ids();
  const int64_t macro_block_cnt = macro_block_ids.count();
  if (macro_block_cnt <= 0) {
    common::ObStoreRange range;
    range.set_whole_range();
    if (OB_FAIL(ranges.push_back(range))) {
      STORAGE_LOG(WARN, "Failed to push range", K(ret), K(ranges), K(range));
    }
  } else {
    common::ObStoreRowkey rowkey;
    common::ObStoreRange range;
    range.get_end_key().set_min();
    range.set_left_open();
    range.set_right_closed();
    blocksstable::ObFullMacroBlockMeta blk_meta;
    int64_t num_blocks_per_range = DEFAULT_MACRO_BLOCK_CNT_GRANULARITY;
    // generate ranges
    for (int64_t i = 0; OB_SUCC(ret) && i < macro_block_cnt; i += num_blocks_per_range) {
      const int64_t last = i + num_blocks_per_range - 1;
      const blocksstable::MacroBlockId &blk_id = macro_block_ids.at(last < macro_block_cnt ? last : macro_block_cnt - 1);
      if (OB_FAIL(first_sstable_->get_meta(blk_id, blk_meta))) {
        STORAGE_LOG(WARN, "Failed to get macro block meta", K(ret), K(blk_id));
      } else if (OB_UNLIKELY(!blk_meta.is_valid())) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "Unexpected invalid macro block meta", K(ret), K(blk_id));
      } else if (FALSE_IT(rowkey.assign(blk_meta.meta_->endkey_, blk_meta.meta_->rowkey_column_number_))) {
      } else if (OB_FAIL(range.get_end_key().deep_copy(range.get_start_key(), allocator_))) {
        STORAGE_LOG(WARN, "Failed to deep copy rowkey", K(ret), K(range.get_end_key()), K(range.get_start_key()));
      } else if (OB_FAIL(rowkey.deep_copy(range.get_end_key(), allocator_))) {
        STORAGE_LOG(WARN, "Failed to deep copy rowkey", K(ret), K(rowkey), K(range.get_end_key()));
      } else if (OB_FAIL(ranges.push_back(range))) {
        STORAGE_LOG(WARN, "Failed to push range", K(ret), K(ranges), K(range));
      }
    }
    if (OB_SUCC(ret)) {
      common::ObStoreRange &last_range = ranges.at(ranges.count() - 1);
      last_range.get_end_key().set_max();
      last_range.set_right_open();
      if (OB_FAIL(check_contiguous_ranges(ranges))) {
        STORAGE_LOG(WARN, "Failed to check contiguous ranges", K(ret));
      }
    }
  }
  if (OB_SUCC(ret)) {
    STORAGE_LOG(INFO, "Succeeded to get ranges by base sstable", K(macro_block_cnt), K(ranges));
  }

  return ret;
}

// To get row-key ranges by iterating over rows
static int get_ranges_by_iteration(
    common::ObIArray<common::ObStoreRange>& ranges, ObIStoreRowIterator& iter,
    common::ObIAllocator& allocator, const int64_t num_rows_per_range)
{
  int ret = OB_SUCCESS;

  int64_t row_cnt = 0;
  common::ObStoreRange range;
  range.get_end_key().set_min();
  range.set_left_open();
  range.set_right_closed();
  common::ObStoreRowkey rkey;
  const ObStoreRow *row = NULL;
  int count = 0;
  while (OB_SUCC(ret) && OB_SUCC(iter.get_next_row(row))) {
    count++;
    if (count >= num_rows_per_range) {
      row_cnt += count;
      count = 0;
      rkey.assign(row->row_val_.cells_, row->row_val_.count_);
      if (OB_FAIL(range.get_end_key().deep_copy(range.get_start_key(), allocator))) {
        STORAGE_LOG(WARN, "Failed to deep copy rowkey", K(ret), K(range.get_end_key()), K(range.get_start_key()));
      } else if (OB_FAIL(rkey.deep_copy(range.get_end_key(), allocator))) {
        STORAGE_LOG(WARN, "Failed to deep copy rowkey", K(ret), K(rkey), K(range.get_end_key()));
      } else {
        if (OB_FAIL(ranges.push_back(range))) {
          STORAGE_LOG(WARN, "Failed to push range", K(ret));
        }
      }
    }
  }
  row_cnt += count;
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }
  // handle the last range
  if (OB_SUCC(ret)) {
    if (count > 0) {
      if (OB_FAIL(range.get_end_key().deep_copy(range.get_start_key(), allocator))) {
        STORAGE_LOG(WARN, "Failed to deep copy rowkey", K(ret));
      } else {
        range.get_end_key().set_max();
        range.set_right_open();
        if (OB_FAIL(ranges.push_back(range))) {
          STORAGE_LOG(WARN, "Failed to push range", K(ret));
        }
      }
    } else if (ranges.empty()) {
      range.set_whole_range();
      if (OB_FAIL(ranges.push_back(range))) {
        STORAGE_LOG(WARN, "Failed to push range", K(ret));
      }
    } else {
      ranges.at(ranges.count() - 1).get_end_key().set_max();
      ranges.at(ranges.count() - 1).set_right_open();
    }
  }

  return ret;
}

int ObParallelMergeCtx::prepare_table_access_param(ObSSTableMergeCtx& merge_ctx,
    common::ObIArray<share::schema::ObColDesc>& rowkey_col_ids,
    common::ObIArray<share::schema::ObColumnParam *>& out_cols_param,
    common::ObIArray<int32_t>& out_cols_project, ObTableAccessParam& tbl_xs_param)
{
  int ret = OB_SUCCESS;

  const uint64_t table_id = merge_ctx.table_schema_->get_table_id();

  if (OB_FAIL(merge_ctx.table_schema_->get_rowkey_column_ids(rowkey_col_ids))) {
    STORAGE_LOG(WARN, "Failed to get rowkey column ids", K(ret));
  } else if (OB_FAIL(tbl_xs_param.init(table_id, merge_ctx.schema_version_,
      merge_ctx.table_schema_->get_rowkey_column_num(), rowkey_col_ids
  ))) {
    STORAGE_LOG(WARN, "Failed to init table access param", K(ret));
  } else {
    // to construct out_cols_param and out_cols_project
    ObColumnParam *buf = static_cast<ObColumnParam *>(allocator_.alloc(rowkey_col_ids.count() * sizeof(ObColumnParam)));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      STORAGE_LOG(WARN, "Failed to allocate memory for ObColumnParam", K(ret));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_col_ids.count(); i++) {
      const ObColumnSchemaV2 *col = merge_ctx.table_schema_->get_column_schema(rowkey_col_ids.at(i).col_id_);
      if (OB_FAIL(out_cols_project.push_back(static_cast<int32_t>(i)))) {
        STORAGE_LOG(WARN, "Failed to push column project", K(ret));
      } else if (OB_ISNULL(col)) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "Failed to get column schema", K(ret), K(rowkey_col_ids.at(i).col_id_));
      } else {
        ObColumnParam *col_param = new (buf + i) ObColumnParam(allocator_);
        if (OB_FAIL(ObTableParam::convert_column_schema_to_param(*col, *col_param))) {
          STORAGE_LOG(WARN, "Failed to convert column schema to param", K(ret));
        } else if (OB_FAIL(out_cols_param.push_back(col_param))) {
          STORAGE_LOG(WARN, "Failed to push param", K(ret));
        }
      }
    }
    if (OB_SUCC(ret)) {
      tbl_xs_param.iter_param_.out_cols_project_ = &out_cols_project;
      tbl_xs_param.out_cols_param_ = &out_cols_param;
      tbl_xs_param.reserve_cell_cnt_ = out_cols_project.count();
    }
  }

  return ret;
}

int ObParallelMergeCtx::prepare_store_ctx(ObSSTableMergeCtx& merge_ctx,
    memtable::ObIMemtableCtxFactory*& memctx_factory, ObStoreCtx& store_ctx)
{
  int ret = OB_SUCCESS;

  const uint64_t table_id = merge_ctx.table_schema_->get_table_id();
  const uint64_t tenant_id = extract_tenant_id(table_id);
  transaction::ObTransService *trx_service = NULL;
  if (OB_ISNULL(trx_service = ObPartitionService::get_instance().get_trans_service())) {
    ret = OB_ERR_SYS;
    STORAGE_LOG(ERROR, "Failed to get transaction service", K(ret));
  } else if (OB_ISNULL(memctx_factory = trx_service->get_mem_ctx_factory())) {
    ret = OB_ERR_SYS;
    STORAGE_LOG(ERROR, "Failed to get mem ctx factory", K(ret));
  } else if (OB_ISNULL(store_ctx.mem_ctx_ = memctx_factory->alloc())) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    STORAGE_LOG(WARN, "Failed to allocate memory context for table access context", K(ret));
  } else if (OB_FAIL(store_ctx.mem_ctx_->trans_begin())) {
    STORAGE_LOG(WARN, "Failed to begin transaction", K(ret));
  } else if (OB_FAIL(store_ctx.mem_ctx_->sub_trans_begin(
      merge_ctx.sstable_version_range_.snapshot_version_,  // snapshot
      INT64_MAX - 2,  // abs_expired_time
      true  // is_safe_snapshot
  ))) {
    STORAGE_LOG(WARN, "Failed to begin sub-transaction", K(ret));
  } else if (OB_FAIL(store_ctx.init_trans_ctx_mgr(merge_ctx.param_.pg_key_))) {
    STORAGE_LOG(WARN, "Failed to init tranx ctx mgr", K(ret), K(merge_ctx.param_.pg_key_));
  }

  return ret;
}

void ObParallelMergeCtx::revert_store_ctx(
    memtable::ObIMemtableCtxFactory* memctx_factory, ObStoreCtx& store_ctx)
{
  if (memctx_factory != NULL && store_ctx.mem_ctx_ != NULL) {
    store_ctx.mem_ctx_->trans_end(true, 0);
    store_ctx.mem_ctx_->trans_clear();
    memctx_factory->free(store_ctx.mem_ctx_);
    store_ctx.mem_ctx_ = NULL;
  }
}

int ObParallelMergeCtx::prepare_table_access_context(
    ObSSTableMergeCtx& merge_ctx, ObStoreCtx& store_ctx, common::ObQueryFlag& query_flag,
    blocksstable::ObBlockCacheWorkingSet& blk_cache_ws, ObTableAccessContext& tbl_xs_ctx)
{
  int ret = OB_SUCCESS;

  const uint64_t table_id = merge_ctx.table_schema_->get_table_id();
  const uint64_t tenant_id = extract_tenant_id(table_id);
  if (OB_FAIL(blk_cache_ws.init(tenant_id))) {
    STORAGE_LOG(WARN, "Failed to init block cache working set", K(ret), K(tenant_id));
  } else if (OB_FAIL(tbl_xs_ctx.init(
      query_flag, store_ctx, allocator_, allocator_, blk_cache_ws, merge_ctx.sstable_version_range_
  ))) {
    STORAGE_LOG(WARN, "Failed to init table access context", K(ret));
  } else {
    tbl_xs_ctx.pkey_ = merge_ctx.param_.pkey_;
  }

  return ret;
}

int ObParallelMergeCtx::prepare_get_table_param(ObSSTableMergeCtx& merge_ctx,
    ObTablesHandle& tbls_handle, ObGetTableParam& get_tbl_param)
{
  int ret = OB_SUCCESS;

  for (int64_t i = 1; OB_SUCC(ret) && i < merge_ctx.tables_handle_.get_count(); i++) {
    if (OB_FAIL(tbls_handle.add_table(merge_ctx.tables_handle_.get_table(i)))) {
      STORAGE_LOG(WARN, "Failed to add table to inc handle", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    get_tbl_param.tables_handle_ = &(tbls_handle);
  }

  return ret;
}


int ObParallelMergeCtx::get_ranges_by_inc_data(
    ObSSTableMergeCtx& merge_ctx, common::ObIArray<common::ObStoreRange>& ranges)
{
  int ret = OB_SUCCESS;

  const uint64_t table_id = merge_ctx.table_schema_->get_table_id();
  if (OB_UNLIKELY(merge_ctx.tables_handle_.get_count() <= 1)) {
    // no inc sstabale
    common::ObStoreRange range;
    range.set_whole_range();
    if (OB_FAIL(ranges.push_back(range))) {
      STORAGE_LOG(WARN, "Failed to push range to ranges", K(ret), K(ranges), K(range));
    } else {
      STORAGE_LOG(INFO, "No inc sstable", K(table_id), K(merge_ctx.tables_handle_.get_count()), K(ranges));
    }
  } else {
    common::ObArray<share::schema::ObColDesc> rowkey_col_ids;
    common::ObArray<share::schema::ObColumnParam *> out_cols_param;
    common::ObArray<int32_t> out_cols_project;
    ObTableAccessParam tbl_xs_param;

    memtable::ObIMemtableCtxFactory *memctx_factory = NULL;
    ObStoreCtx store_ctx;

    common::ObQueryFlag query_flag(common::ObQueryFlag::Forward, true, true, true, false, false, false);
    blocksstable::ObBlockCacheWorkingSet blk_cache_ws;
    ObTableAccessContext tbl_xs_ctx;

    ObTablesHandle inc_handle;
    ObGetTableParam get_tbl_param;

    ObMultipleScanMerge mpl_scan_mrg;
    mpl_scan_mrg.set_iter_del_row(true);

    common::ObExtStoreRange range_to_scan;
    range_to_scan.get_range().set_whole_range();

    // to calculate num_rows_per_range
    int64_t base_macro_blk_cnt = first_sstable_->get_macro_block_ids().count();
    int64_t num_blks_per_range = DEFAULT_MACRO_BLOCK_CNT_GRANULARITY;
    int64_t num_rows_per_range = DEFAULT_ROW_SCAN_GRANULARITY;
    if (base_macro_blk_cnt > num_blks_per_range) {
      num_rows_per_range = first_sstable_->get_total_row_count() / base_macro_blk_cnt * num_blks_per_range;
      if (num_rows_per_range <= 0) {
        num_rows_per_range = DEFAULT_ROW_SCAN_GRANULARITY;
      }
    }

    if (OB_FAIL(prepare_table_access_param(
        merge_ctx, rowkey_col_ids, out_cols_param, out_cols_project, tbl_xs_param
    ))) {
      STORAGE_LOG(WARN, "Failed to prepare table access param", K(ret));
    } else if (OB_FAIL(prepare_store_ctx(merge_ctx, memctx_factory, store_ctx))) {
      STORAGE_LOG(WARN, "Failed to prepare store ctx", K(ret));
    } else if (OB_FAIL(prepare_table_access_context(
        merge_ctx, store_ctx, query_flag, blk_cache_ws, tbl_xs_ctx
    ))) {
      STORAGE_LOG(WARN, "Failed to prepare table access context", K(ret));
    } else if (OB_FAIL(prepare_get_table_param(merge_ctx, inc_handle, get_tbl_param))) {
      STORAGE_LOG(WARN, "Failed to prepare get table param", K(ret));
    } else if (OB_FAIL(mpl_scan_mrg.init(tbl_xs_param, tbl_xs_ctx, get_tbl_param))) {
      STORAGE_LOG(WARN, "Failed to init multiple scan merge", K(ret));
    } else if (OB_FAIL(mpl_scan_mrg.open(range_to_scan))) {
      STORAGE_LOG(WARN, "Failed to open multiple scan merge", K(ret));
    } else if (OB_FAIL(get_ranges_by_iteration(
        ranges, mpl_scan_mrg, allocator_, num_rows_per_range
    ))) {
      STORAGE_LOG(WARN, "Failed to get ranges", K(ret));
    } else if (OB_FAIL(check_contiguous_ranges(ranges))) {
      STORAGE_LOG(WARN, "Failed to check contiguous ranges", K(ret));
    } else {
      STORAGE_LOG(INFO, "Succeeded to get ranges by inc data", K(table_id), K(ranges));
    }
    revert_store_ctx(memctx_factory, store_ctx);
  }

  return ret;
}

// generate ranges according to both <base_ranges> and <inc_ranges>
static int combine_ranges(
    const common::ObIArray<common::ObStoreRange>& base_ranges,
    const common::ObIArray<common::ObStoreRange>& inc_ranges,
    common::ObIArray<common::ObStoreRange>& result_ranges)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(base_ranges.count() < 2 || inc_ranges.count() < 2)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid args", K(ret), K(base_ranges), K(inc_ranges));
  } else {
    int64_t i = 0, j = 0;
    common::ObStoreRange range;
    range.get_end_key().set_min();
    range.set_left_open();
    range.set_right_closed();
    int cmp = 0;
    const common::ObStoreRowkey *ek;
    while (OB_SUCC(ret) && i < base_ranges.count() && j < inc_ranges.count()) {
      const common::ObStoreRowkey& ek1 = base_ranges.at(i).get_end_key();
      const common::ObStoreRowkey& ek2 = inc_ranges.at(j).get_end_key();
      if (OB_FAIL(ek1.compare(ek2, cmp))) {
        STORAGE_LOG(WARN, "Failed to compare rowkeys", K(ret), K(ek1), K(ek2));
      } else if (cmp < 0) {
        ek = &ek1; i++;
      } else if (cmp > 0) {
        ek = &ek2; j++;
      } else {
        ek = &ek1; i++; j++;
      }
      if (OB_SUCC(ret)) {
        range.set_start_key(range.get_end_key());
        range.set_end_key(*ek);
        if (OB_FAIL(result_ranges.push_back(range))) {
          STORAGE_LOG(WARN, "Failed to push range into result ranges", K(ret), K(range));
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    result_ranges.at(result_ranges.count() - 1).set_right_open();
    if (OB_FAIL(check_contiguous_ranges(result_ranges))) {
      STORAGE_LOG(WARN, "Failed to check contiguous ranges", K(ret));
    }
  }

  return ret;
}

// Combine multiple contiguous ranges (one thread should handle those ranges)
// according to number of available threads.
static int merge_contiguous_ranges(const common::ObIArray<common::ObStoreRange>& ranges,
    common::ObIArray<common::ObExtStoreRange>& result_ranges, common::ObIAllocator& allocator)
{
  int ret = OB_SUCCESS;

  const int64_t merge_thread_count = (GCONF.merge_thread_count == 0) ? 10 : GCONF.merge_thread_count;
  if (ranges.count() <= merge_thread_count) {
    for (int64_t i = 0; OB_SUCC(ret) && i < ranges.count(); i++) {
      common::ObExtStoreRange ext_range(ranges.at(i));
      if (OB_FAIL(ext_range.to_collation_free_range_on_demand_and_cutoff_range(allocator))) {
        STORAGE_LOG(WARN, "Failed to transform and cut off range", K(ret));
      } else if (OB_FAIL(result_ranges.push_back(ext_range))) {
        STORAGE_LOG(WARN, "Failed to push merge range", K(ret), K(ext_range));
      }
    }
  } else {
    int64_t num_ranges_per_thread = (ranges.count() + merge_thread_count - 1) / merge_thread_count;
    common::ObStoreRange merged_range;
    for (int64_t i = 0; OB_SUCC(ret) && i < ranges.count(); i += num_ranges_per_thread) {
      const common::ObStoreRange &first_range = ranges.at(i);
      int64_t last = i + num_ranges_per_thread - 1;
      const common::ObStoreRange &last_range = ranges.at(last < ranges.count() ? last : ranges.count() - 1);
      merged_range.reset();
      merged_range.set_start_key(first_range.get_start_key());
      merged_range.set_left_open();
      merged_range.set_end_key(last_range.get_end_key());
      merged_range.set_right_closed();
      common::ObExtStoreRange ext_range(merged_range);
      if (OB_FAIL(ext_range.to_collation_free_range_on_demand_and_cutoff_range(allocator))) {
        STORAGE_LOG(WARN, "Failed to transform and cut off range", K(ret));
      } else if (OB_FAIL(result_ranges.push_back(ext_range))) {
        STORAGE_LOG(WARN, "Failed to push back merge range to array", K(ret), K(ext_range));
      }
    }
    if (OB_SUCC(ret)) {
      (result_ranges.at(result_ranges.count() - 1)).get_range().set_right_open();
    }
  }

  return ret;
}

int ObParallelMergeCtx::prepare_range_array_for_parallel_major_merge(ObSSTableMergeCtx& merge_ctx)
{
  int ret = OB_SUCCESS;

  const uint64_t table_id = merge_ctx.table_schema_->get_table_id();
  const common::ObPartitionKey pkey = merge_ctx.param_.pkey_;
  common::ObArray<common::ObStoreRange> inc_ranges, base_ranges, combined_ranges;
  const common::ObIArray<common::ObStoreRange> *ranges;
  const int64_t merge_thread_count = (GCONF.merge_thread_count == 0) ? 10 : GCONF.merge_thread_count;
  if (OB_FAIL(get_ranges_by_inc_data(merge_ctx, inc_ranges))) {
    STORAGE_LOG(WARN, "Failed to get ranges by inc data", K(ret));
  } else if (merge_ctx.is_full_merge_ || inc_ranges.count() < merge_thread_count / 2) {
    if (OB_FAIL(get_ranges_by_base_sstable(base_ranges))) {
      STORAGE_LOG(WARN, "Failed to get ranges by base sstable", K(ret));
    } else if (inc_ranges.count() < 2) {
      ranges = &base_ranges;
    } else if (base_ranges.count() < 2) {
      ranges = &inc_ranges;
    } else if (OB_FAIL(combine_ranges(base_ranges, inc_ranges, combined_ranges))) {
      STORAGE_LOG(WARN, "Failed to combine base and inc ranges", K(ret));
    } else {
      ranges = &combined_ranges;
    }
  } else {
    ranges = &inc_ranges;
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(merge_contiguous_ranges(*ranges, range_array_, allocator_))) {
      STORAGE_LOG(WARN, "Failed to merge contiguous ranges", K(ret));
    } else {
      concurrent_cnt_ = range_array_.count();
      parallel_type_ = PARALLEL_MAJOR;
      STORAGE_LOG(INFO, "Succeeded to prepare range array for parallel major merge",
          K(table_id), K(concurrent_cnt_), K(range_array_));
    }
  }

  return ret;
}

int ObParallelMergeCtx::init_parallel_major_merge(ObSSTableMergeCtx& merge_ctx)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(MAJOR_MERGE != merge_ctx.param_.schedule_merge_type_)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to init parallel major merge", K(ret), K(merge_ctx));
  } else if (OB_ISNULL(merge_ctx.tables_handle_.get_table(0))) {
    ret = OB_ERR_SYS;
    STORAGE_LOG(WARN, "Unexpected null first table", K(ret), K(merge_ctx.tables_handle_));
  } else if (OB_UNLIKELY(!merge_ctx.tables_handle_.get_table(0)->is_sstable())) {
    ret = OB_ERR_SYS;
    STORAGE_LOG(WARN, "First table must be sstable", K(ret), K(merge_ctx.tables_handle_));
  } else {
    // the first sstable is the baseline sstable
    first_sstable_ = static_cast<ObSSTable*>(merge_ctx.tables_handle_.get_table(0));
    const int64_t tablet_size = merge_ctx.table_schema_->get_tablet_size();
    if (0 == tablet_size) {
      major_merge_range_split_use_new_way_ = true;
      if (OB_FAIL(prepare_range_array_for_parallel_major_merge(merge_ctx))) {
        STORAGE_LOG(WARN, "Failed to prepare range array", K(ret));
      }
    } else {
      if (OB_FAIL(first_sstable_->get_concurrent_cnt(tablet_size, concurrent_cnt_))) {
        STORAGE_LOG(WARN, "Failed to get concurrent cnt from first sstable", K(ret), K(tablet_size), K_(concurrent_cnt));
      } else {
        parallel_type_ = PARALLEL_MAJOR;
      }
    }
  }

  return ret;
}

int ObParallelMergeCtx::init_parallel_mini_merge(ObSSTableMergeCtx& merge_ctx)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(MINI_MERGE != merge_ctx.param_.schedule_merge_type_)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to init parallel mini minor merge", K(ret), K(merge_ctx));
  } else {
    const int64_t tablet_size = merge_ctx.table_schema_->get_tablet_size();
    memtable::ObMemtable* memtable = nullptr;
    if (OB_FAIL(merge_ctx.tables_handle_.get_first_memtable(memtable))) {
      STORAGE_LOG(WARN, "failed to get first memtable", K(ret), "merge tables", merge_ctx.tables_handle_);
    } else {
      int64_t total_bytes = 0;
      int64_t total_rows = 0;
      if (OB_FAIL(memtable->estimate_phy_size(
              merge_ctx.table_schema_->get_table_id(), nullptr, nullptr, total_bytes, total_rows))) {
        STORAGE_LOG(WARN, "Failed to get estimate size from memtable", K(ret));
      } else {
        int64_t mini_merge_thread = GCONF._mini_merge_concurrency;
        ObArray<ObStoreRange> store_ranges;
        mini_merge_thread = MAX(mini_merge_thread, PARALLEL_MERGE_TARGET_TASK_CNT);
        concurrent_cnt_ = MIN((total_bytes + tablet_size - 1) / tablet_size, mini_merge_thread);
        if (concurrent_cnt_ <= 1) {
          if (OB_FAIL(init_serial_merge())) {
            STORAGE_LOG(WARN, "Failed to init serialize merge", K(ret));
          }
        } else if (OB_FAIL(memtable->get_split_ranges(
                       merge_ctx.table_schema_->get_table_id(), nullptr, nullptr, concurrent_cnt_, store_ranges))) {
          if (OB_ENTRY_NOT_EXIST == ret) {
            if (OB_FAIL(init_serial_merge())) {
              STORAGE_LOG(WARN, "Failed to init serialize merge", K(ret));
            }
          } else {
            STORAGE_LOG(WARN, "Failed to get split ranges from memtable", K(ret));
          }
        } else if (OB_UNLIKELY(store_ranges.count() != concurrent_cnt_)) {
          ret = OB_ERR_UNEXPECTED;
          STORAGE_LOG(WARN, "Unexpected range array and concurrent_cnt", K(ret), K_(concurrent_cnt), K(store_ranges));
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < store_ranges.count(); i++) {
            ObExtStoreRange ext_range(store_ranges.at(i));
            if (OB_FAIL(ext_range.to_collation_free_range_on_demand_and_cutoff_range(allocator_))) {
              STORAGE_LOG(WARN, "Failed to transform and cut off range", K(ret));
            } else if (OB_FAIL(range_array_.push_back(ext_range))) {
              STORAGE_LOG(WARN, "Failed to push back merge range to array", K(ret), K(ext_range));
            }
          }
          parallel_type_ = PARALLEL_MINI;
          STORAGE_LOG(INFO, "Succ to get parallel mini merge ranges", K_(concurrent_cnt), K_(range_array));
        }
      }
    }
  }

  return ret;
}

int ObParallelMergeCtx::init_parallel_mini_minor_merge(ObSSTableMergeCtx& merge_ctx)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!merge_ctx.param_.is_minor_merge())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to init parallel mini minor merge", K(ret), K(merge_ctx));
  } else {
    const int64_t tablet_size = merge_ctx.table_schema_->get_tablet_size();
    ObRangeSplitInfo range_info;
    ObArray<ObSSTable*> sstables;
    ObArray<ObStoreRange> store_ranges;
    ObPartitionRangeSpliter range_spliter;
    ObStoreRange whole_range;
    whole_range.set_whole_range();
    if (OB_FAIL(merge_ctx.tables_handle_.get_all_sstables(sstables))) {
      STORAGE_LOG(WARN, "Failed to get all sstables from merge ctx", K(ret), K(merge_ctx));
    } else if (sstables.count() != merge_ctx.tables_handle_.get_count()) {
      if (OB_FAIL(init_serial_merge())) {
        STORAGE_LOG(WARN, "Failed to init serialize merge", K(ret));
      } else if (merge_ctx.param_.schedule_merge_type_ == MINI_MINOR_MERGE) {
        STORAGE_LOG(WARN, "Unexpected tables handle for mini minor merge", K(ret), K(merge_ctx.tables_handle_));
      }
    } else if (OB_FAIL(range_spliter.get_range_split_info(sstables, whole_range, range_info))) {
      STORAGE_LOG(WARN, "Failed to init range spliter", K(ret));
    } else if (OB_FAIL(calc_mini_minor_parallel_degree(
                   tablet_size, range_info.total_size_, sstables.count(), range_info.parallel_target_count_))) {
      STORAGE_LOG(WARN, "Failed to calc mini minor parallel degree", K(ret));
    } else if (range_info.parallel_target_count_ <= 1) {
      if (OB_FAIL(init_serial_merge())) {
        STORAGE_LOG(WARN, "Failed to init serialize merge", K(ret));
      }
    } else if (OB_FAIL(range_spliter.split_ranges(range_info, allocator_, true, store_ranges))) {
      STORAGE_LOG(WARN, "Failed to split parallel ranges", K(ret));
    } else if (OB_UNLIKELY(store_ranges.count() <= 1)) {
      range_spliter.reset();
      reset();
      if (OB_FAIL(init_serial_merge())) {
        STORAGE_LOG(WARN, "Failed to init serialize merge", K(ret));
      } else {
        STORAGE_LOG(INFO, "parallel minor merge back to serialize merge");
      }
    } else {
      concurrent_cnt_ = store_ranges.count();
      parallel_type_ = PARALLEL_MINI_MINOR;
      STORAGE_LOG(INFO, "Succ to get parallel mini minor merge ranges", K_(concurrent_cnt), K_(range_array));
      for (int64_t i = 0; OB_SUCC(ret) && i < store_ranges.count(); i++) {
        ObExtStoreRange ext_range(store_ranges.at(i));
        if (OB_FAIL(ext_range.to_collation_free_range_on_demand_and_cutoff_range(allocator_))) {
          STORAGE_LOG(WARN, "Failed to transform and cut off range", K(ret));
        } else if (OB_FAIL(range_array_.push_back(ext_range))) {
          STORAGE_LOG(WARN, "Failed to push back merge range to array", K(ret), K(ext_range));
        }
      }
    }
  }
  return ret;
}

int ObParallelMergeCtx::calc_mini_minor_parallel_degree(
    const int64_t tablet_size, const int64_t total_size, const int64_t sstable_count, int64_t& parallel_degree)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(tablet_size == 0 || total_size < 0 || sstable_count <= 1)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN,
        "Invalid argument to calc mini minor parallel degree",
        K(ret),
        K(tablet_size),
        K(total_size),
        K(sstable_count));
  } else {
    int64_t minor_merge_thread = GCONF.minor_merge_concurrency;
    int64_t avg_sstable_size = total_size / sstable_count;
    parallel_degree = MIN(
        MAX(minor_merge_thread, PARALLEL_MERGE_TARGET_TASK_CNT), (avg_sstable_size + tablet_size - 1) / tablet_size);
  }

  return ret;
}

}  // namespace storage
}  // namespace oceanbase
