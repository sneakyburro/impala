// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exec/topn-node.h"

using namespace impala;

void TopNNode::InsertBatch(RowBatch* batch) {
  // TODO: after inlining the comparator calls with codegen - IMPALA-4065 - we could
  // probably squeeze more performance out of this loop by ensure that as many loads
  // are hoisted out of the loop as possible (either via code changes or __restrict__)
  // annotations.
  FOREACH_ROW(batch, 0, iter) {
    int num_to_reclaim = heap_->InsertTupleRow(this, iter.Get());
    // Using a branch here instead of adding the value directly improves
    // top-n performance by a couple of %.
    if (num_to_reclaim != 0) rows_to_reclaim_ += num_to_reclaim;
  }
}

int TopNNode::Heap::InsertTupleRow(TopNNode* node, TupleRow* input_row) {
  const TupleDescriptor& tuple_desc = *node->output_tuple_desc_;
  if (priority_queue_.Size() < heap_capacity()) {
    // Add all tuples (including any ties) until we hit capacity.
    Tuple* insert_tuple = reinterpret_cast<Tuple*>(
        node->tuple_pool_->Allocate(node->tuple_byte_size()));
    insert_tuple->MaterializeExprs<false, false>(input_row, tuple_desc,
        node->output_tuple_expr_evals_, node->tuple_pool_.get());

    priority_queue_.Push(insert_tuple);
    return 0;
  }

  // We're at capacity - compare to the first row in the priority queue to see if
  // we need to insert this row into the queue.
  DCHECK(!priority_queue_.Empty());
  Tuple* top_tuple = priority_queue_.Top();
  node->tmp_tuple_->MaterializeExprs<false, true>(input_row, tuple_desc,
      node->output_tuple_expr_evals_, nullptr);
  if (include_ties()) {
    return InsertTupleWithTieHandling(node, node->tmp_tuple_);
  } else {
    if (node->tuple_row_less_than_->Less(node->tmp_tuple_, top_tuple)) {
      // Pop off the old head, and replace with the new tuple. Deep copy into 'top_tuple'
      // to reuse the fixed-length memory of 'top_tuple'.
      node->tmp_tuple_->DeepCopy(top_tuple, tuple_desc, node->tuple_pool_.get());
      // Re-heapify from the top element and down.
      priority_queue_.HeapifyFromTop();
      return 1;
    }
    return 0;
  }
}

int TopNNode::Heap::InsertTupleWithTieHandling(
    TopNNode* node, Tuple* materialized_tuple) {
  DCHECK(include_ties());
  DCHECK_EQ(capacity_, priority_queue_.Size())
        << "Ties only need special handling when heap is at capacity";
  const TupleDescriptor& tuple_desc = *node->output_tuple_desc_;
  Tuple* top_tuple = priority_queue_.Top();
  // If we need to retain ties with the current head, the logic is more complex - we
  // have a logical heap in indices [0, heap_capacity()) of 'priority_queue_' plus
  // some number of tuples in 'overflowed_ties_' that are equal to priority_queue_.Top()
  // according to 'intra_partition_tuple_row_less_than_'.
  int cmp_result = node->tuple_row_less_than_->Compare(materialized_tuple, top_tuple);
  if (cmp_result == 0) {
    // This is a duplicate of the current min, we need to include it as a tie with min.
    Tuple* insert_tuple =
        reinterpret_cast<Tuple*>(node->tuple_pool_->Allocate(node->tuple_byte_size()));
    materialized_tuple->DeepCopy(insert_tuple, tuple_desc, node->tuple_pool_.get());
    overflowed_ties_.push_back(insert_tuple);
    return 0;
  } else if (cmp_result > 0) {
    // Tuple does not belong in the heap.
    return 1;
  } else {
    // 'materialized_tuple' needs to be added. Figure out which other tuples, if any,
    // need to be removed from the heap.
    DCHECK_LT(cmp_result, 0);
    // Pop off the head.
    priority_queue_.Pop();

    // Check if 'top_tuple' (the tuple we just popped off) is tied with the new head.
    if (heap_capacity() > 1 &&
        node->tuple_row_less_than_->Compare(top_tuple, priority_queue_.Top()) == 0) {
      // The new top is still tied with the tuples in 'overflowed_ties_' so we must keep
      // it. The previous top becomes another overflowed tuple.
      overflowed_ties_.push_back(top_tuple);
      Tuple* insert_tuple =
          reinterpret_cast<Tuple*>(node->tuple_pool_->Allocate(node->tuple_byte_size()));
      materialized_tuple->DeepCopy(insert_tuple, tuple_desc, node->tuple_pool_.get());
      // Push the new tuple onto the heap, but retain the tied tuple.
      priority_queue_.Push(insert_tuple);
      return 0;
    } else {
      // No tuples tied with 'top_tuple' are left.
      // Reuse the fixed-length memory of 'top_tuple' to reduce allocations.
      int64_t num_rows_replaced = overflowed_ties_.size() + 1;
      overflowed_ties_.clear();
      materialized_tuple->DeepCopy(top_tuple, tuple_desc, node->tuple_pool_.get());
      priority_queue_.Push(top_tuple);
      return num_rows_replaced;
    }
  }
}

