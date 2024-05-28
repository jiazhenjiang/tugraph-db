/**
 * Copyright 2022 AntGroup CO., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#pragma once

#include "cypher/execution_plan/ops/op.h"

namespace cypher {

class OpGqlDelete : public OpBase {
    const std::vector<std::string>& items_;
    std::vector<lgraph::VertexId> vertices_to_delete_;
    std::vector<lgraph::EdgeUid> edges_to_delete_;
    bool summary_ = false;

    void CollectVEToDelete() {
        for (auto &item : items_) {
            auto it = record->symbol_table->symbols.find(item);
            CYPHER_THROW_ASSERT(it != record->symbol_table->symbols.end());
            if (it->second.type == SymbolNode::NODE) {
                auto node = record->values[it->second.id].node;
                if (node && node->PullVid() >= 0) {
                    vertices_to_delete_.emplace_back(node->PullVid());
                    /* TODO(anyone) if buffer size overflow, do this task with
                     * optimized op. dump ids to delete into tmp file, note that
                     * manage file with object and delete file in destructor,
                     * make sure the tmp files removed after exception occurs.
                     */
                    if (vertices_to_delete_.size() > DELETE_BUFFER_SIZE) {
                        CYPHER_TODO();
                    }
                }
            } else if (it->second.type == SymbolNode::RELATIONSHIP) {
                auto relp = record->values[it->second.id].relationship;
                if (relp && relp->ItRef() && relp->ItRef()->IsValid()) {
                    edges_to_delete_.emplace_back(relp->ItRef()->GetUid());
                    if (edges_to_delete_.size() > DELETE_BUFFER_SIZE) {
                        CYPHER_TODO();
                    }
                }
            } else {
                CYPHER_TODO();
            }
        }
    }

    void DoDeleteVE(RTContext *ctx) {
        LOG_DEBUG() << "vertices & edges to delete:";
        for (auto &v : vertices_to_delete_) LOG_DEBUG() << "V[" << v << "]";
        for (auto &e : edges_to_delete_) LOG_DEBUG() << "E[" << _detail::EdgeUid2String(e) << "]";

        for (auto &e : edges_to_delete_) {
            if (ctx->txn_->GetTxn()->DeleteEdge(e)) {
                ctx->result_info_->statistics.edges_deleted++;
            }
        }
        for (auto &v : vertices_to_delete_) {
            size_t n_in, n_out;
            if (ctx->txn_->GetTxn()->DeleteVertex(v, &n_in, &n_out)) {
                ctx->result_info_->statistics.vertices_deleted++;
                ctx->result_info_->statistics.edges_deleted += n_in + n_out;
            }
        }
        /* NOTE:
         * lgraph::EdgeIterator will refresh itself after calling
         * Delete(), and point to the next element.
         * While lgraph::Transaction::DeleteEdge() will not refresh
         * the iterator, after calling this method, the edge iterator
         * just becomes invalid.  */
        ctx->txn_->GetTxn()->RefreshIterators();
    }

    void ResultSummary(RTContext *ctx) {
        if (summary_) {            
            std::string summary;
            summary.append("deleted ")
                .append(std::to_string(ctx->result_info_->statistics.vertices_deleted))
                .append(" vertices, deleted ")
                .append(std::to_string(ctx->result_info_->statistics.edges_deleted))
                .append(" edges.");
            auto header = ctx->result_->Header();
            header.emplace_back(std::make_pair("<SUMMARY>", lgraph_api::LGraphType::STRING));
            ctx->result_->ResetHeader(header);
            CYPHER_THROW_ASSERT(record);
            record->values.clear();
            record->AddConstant(lgraph::FieldData(summary));
        } else {
            /* There should be a "Project" operation on top of this "Delete",
             * so leave result set to it. */
        }
    }

 public:
    OpGqlDelete(const std::vector<std::string>& items)
        : OpBase(OpType::GQL_DELETE_, "Delete")
        , items_(items) {
        state = StreamUnInitialized;
    }

    OpResult Initialize(RTContext *ctx) override {
        CYPHER_THROW_ASSERT(parent && children.size() < 2);
        summary_ = !parent->parent;
        for (auto child : children) {
            child->Initialize(ctx);
        }
        record = children[0]->record;
        return OP_OK;
    }

    OpResult RealConsume(RTContext *ctx) override {
        if (state == StreamDepleted) return OP_DEPLETED;
        if (children.size() != 1) CYPHER_INTL_ERR();
        auto child = children[0];
        /* Avoid reading after writing (RAW) by split reading-while-writing
         * into 2 steps:
         * 1. collect vertices&edges to delete, this op store the
         *    vids/edge_uids while child op move iterator to next;
         * 2. do the delete action;  */
        while (child->Consume(ctx) == OP_OK) CollectVEToDelete();
        DoDeleteVE(ctx);
        ResultSummary(ctx);
        state = StreamDepleted;
        return OP_OK;
    }

    OpResult ResetImpl(bool complete) override {
        state = StreamUnInitialized;
        return OP_OK;
    }

    std::string ToString() const override {
        std::string str(name);
        return str;
    }

    CYPHER_DEFINE_VISITABLE()

    CYPHER_DEFINE_CONST_VISITABLE()
};
}  // namespace cypher
