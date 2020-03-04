/*-------------------------------------------------------------------------
 * Copyright (C) 2019, 4paradigm
 * planner.cc
 *
 * Author: chenjing
 * Date: 2019/10/24
 *--------------------------------------------------------------------------
 **/
#include "plan/planner.h"
#include <proto/common.pb.h>
#include <map>
#include <set>
#include <string>
#include <vector>
namespace fesql {
namespace plan {

int Planner::CreateSelectStmtPlan(const node::SQLNode *select_tree,
                                  PlanNode **plan_tree, Status &status) {
    const node::SelectStmt *root = (const node::SelectStmt *)select_tree;
    const node::NodePointVector &table_ref_list = root->GetTableRefList();
    if (table_ref_list.empty()) {
        status.msg =
            "can not create select plan node with empty table references";
        status.code = common::kSQLError;
        return status.code;
    }

    std::vector<node::PlanNode *> relation_nodes;
    for (node::SQLNode *node : table_ref_list) {
        if (node::kTable != node->GetType()) {
            status.msg = "can not create Relation plan node with sql node " +
                         node::NameOfSQLNodeType(node->GetType());
            status.code = common::kPlanError;
        }
        node::TableNode *table_node = dynamic_cast<node::TableNode *>(node);
        relation_nodes.push_back(node_manager_->MakeRelationNode(table_node));
    }

    // from tables
    auto iter = relation_nodes.cbegin();
    node::PlanNode *current_node = *iter;
    iter++;
    // cross product if there are multi tables
    for (; iter != relation_nodes.cend(); iter++) {
        current_node = node_manager_->MakeCrossProductNode(current_node, *iter);
    }

    // where condition
    if (nullptr != root->where_clause_ptr_) {
        current_node = node_manager_->MakeSelectPlanNode(
            current_node, root->where_clause_ptr_);
    }

    // group by
    if (nullptr != root->group_clause_ptr_) {
        current_node = node_manager_->MakeGroupPlanNode(
            current_node, root->group_clause_ptr_);
    }

    // select target_list

    // prepare project list plan node
    const node::NodePointVector &select_expr_list = root->GetSelectList();
    if (false == select_expr_list.empty()) {
        std::map<node::WindowDefNode *, node::ProjectListPlanNode *>
            project_list_map;
        // prepare window def
        int w_id = 1;
        std::map<std::string, node::WindowDefNode *> windows;
        if (!root->GetWindowList().empty()) {
            for (auto node : root->GetWindowList()) {
                node::WindowDefNode *w =
                    dynamic_cast<node::WindowDefNode *>(node);
                windows[w->GetName()] = w;
            }
        }

        for (uint32_t pos = 0u; pos < select_expr_list.size(); pos++) {
            auto expr = select_expr_list[pos];
            node::ProjectNode *project_node_ptr =
                (node::ProjectNode *)(node_manager_->MakePlanNode(
                    node::kProject));

            CreateProjectPlanNode(expr, pos, project_node_ptr, status);
            if (0 != status.code) {
                return status.code;
            }
            node::WindowDefNode *w_ptr = node::WindowOfExpression(
                windows, project_node_ptr->GetExpression());

            if (project_list_map.find(w_ptr) == project_list_map.end()) {
                if (w_ptr == nullptr) {
                    project_list_map[w_ptr] =
                        node_manager_->MakeProjectListPlanNode(nullptr);
                } else {
                    node::WindowPlanNode *w_node_ptr =
                        node_manager_->MakeWindowPlanNode(w_id++);
                    CreateWindowPlanNode(w_ptr, w_node_ptr, status);
                    if (common::kOk != status.code) {
                        return status.code;
                    }
                    project_list_map[w_ptr] =
                        node_manager_->MakeProjectListPlanNode(nullptr);
                }
            }
//            project_list_map[w_ptr]->AddProject(project_node_ptr);
        }
        //        // add MergeNode if multi ProjectionLists exist
        //        PlanNodeList project_list_vec(w_id);
        //        for (auto &v : project_list_map) {
        //            node::ProjectListPlanNode *project_list = v.second;
        //            int pos = nullptr == project_list->GetW()
        //                      ? 0
        //                      : project_list->GetW()->GetId();
        //            project_list_vec[pos] = project_list;
        //        }
        //        current_node =
        //            node_manager_->MakeProjectPlanNode(current_node,
        //            project_list_vec);
        // set limit
        if (nullptr != root->GetLimit()) {
            const node::LimitNode *limit_ptr =
                (node::LimitNode *)root->GetLimit();
            node::LimitPlanNode *limit_plan_ptr =
                (node::LimitPlanNode *)node_manager_->MakePlanNode(
                    node::kPlanTypeLimit);
            limit_plan_ptr->SetLimitCnt(limit_ptr->GetLimitCount());
            current_node = node_manager_->MakeLimitPlanNode(
                current_node, limit_ptr->GetLimitCount());
        }
    }
    *plan_tree = current_node;
    return true;
}

int64_t Planner::CreateFrameOffset(const node::FrameBound *bound,
                                   Status &status) {
    bool negtive = false;
    switch (bound->GetBoundType()) {
        case node::kCurrent: {
            return 0;
        }
        case node::kPreceding: {
            negtive = true;
            break;
        }
        case node::kFollowing: {
            negtive = false;
            break;
        }
        default: {
            status.msg =
                "cannot create window frame with unrecognized bound type, "
                "only "
                "support CURRENT|PRECEDING|FOLLOWING";
            status.code = common::kUnSupport;
            return -1;
        }
    }
    if (nullptr == bound->GetOffset()) {
        return negtive ? INT64_MIN : INT64_MAX;
    }
    if (node::kExprPrimary != bound->GetOffset()->GetExprType()) {
        status.msg =
            "cannot create window frame, only support "
            "primary frame";
        status.code = common::kTypeError;
        return 0;
    }

    int64_t offset = 0;
    node::ConstNode *primary =
        dynamic_cast<node::ConstNode *>(bound->GetOffset());
    switch (primary->GetDataType()) {
        case node::DataType::kInt16:
            offset = static_cast<int64_t>(primary->GetSmallInt());
            break;
        case node::DataType::kInt32:
            offset = static_cast<int64_t>(primary->GetInt());
            break;
        case node::DataType::kInt64:
            offset = (primary->GetLong());
            break;
        case node::DataType::kDay:
        case node::DataType::kHour:
        case node::DataType::kMinute:
        case node::DataType::kSecond:
            offset = (primary->GetMillis());
            break;
        default: {
            status.msg =
                "cannot create window frame, only support "
                "smallint|int|bigint offset of frame";
            status.code = common::kTypeError;
            return 0;
        }
    }
    return negtive ? -1 * offset : offset;
}

void Planner::CreateWindowPlanNode(
    node::WindowDefNode *w_ptr, node::WindowPlanNode *w_node_ptr,
    Status &status) {  // NOLINT (runtime/references)

    if (nullptr != w_ptr) {
        int64_t start_offset = 0;
        int64_t end_offset = 0;
        if (nullptr != w_ptr->GetFrame()) {
            node::FrameNode *frame =
                dynamic_cast<node::FrameNode *>(w_ptr->GetFrame());
            node::FrameBound *start = frame->GetStart();
            node::FrameBound *end = frame->GetEnd();

            start_offset = CreateFrameOffset(start, status);
            if (common::kOk != status.code) {
                LOG(WARNING)
                    << "fail to create project list node: " << status.msg;
                return;
            }
            end_offset = CreateFrameOffset(end, status);
            if (common::kOk != status.code) {
                LOG(WARNING)
                    << "fail to create project list node: " << status.msg;
                return;
            }

            if (end_offset == INT64_MAX) {
                LOG(WARNING) << "fail to create project list node: end frame "
                                "can't be unbound ";
                return;
            }

            if (w_ptr->GetName().empty()) {
                w_node_ptr->SetName(
                    GenerateName("anonymous_w_", w_node_ptr->GetId()));
            } else {
                w_node_ptr->SetName(w_ptr->GetName());
            }
            w_node_ptr->SetStartOffset(start_offset);
            w_node_ptr->SetEndOffset(end_offset);
            w_node_ptr->SetIsRangeBetween(node::kFrameRange ==
                                          frame->GetFrameType());
            w_node_ptr->SetKeys(w_ptr->GetPartitions());
            w_node_ptr->SetOrders(w_ptr->GetOrders());
        } else {
            LOG(WARNING) << "fail to create project list node: right frame "
                            "can't be unbound ";
            return;
        }
    }
}

void Planner::CreateProjectPlanNode(
    const SQLNode *root, const uint32_t pos, node::ProjectNode *plan_tree,
    Status &status) {  // NOLINT (runtime/references)
    if (nullptr == root) {
        status.msg = "fail to create project node: query tree node it null";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return;
    }

    switch (root->GetType()) {
        case node::kResTarget: {
            const node::ResTarget *target_ptr = (const node::ResTarget *)root;

            if (target_ptr->GetName().empty()) {
                plan_tree->SetName(target_ptr->GetVal()->GetExprString());
            } else {
                plan_tree->SetName(target_ptr->GetName());
            }
            plan_tree->SetPos(pos);
            plan_tree->SetExpression(target_ptr->GetVal());
            return;
        }
        default: {
            status.msg = "can not create project plan node with type " +
                         node::NameOfSQLNodeType(root->GetType());
            status.code = common::kPlanError;
            LOG(WARNING) << status.msg;
            return;
        }
    }
}

void Planner::CreateCreateTablePlan(
    const node::SQLNode *root, node::CreatePlanNode *plan_tree,
    Status &status) {  // NOLINT (runtime/references)
    const node::CreateStmt *create_tree = (const node::CreateStmt *)root;
    plan_tree->SetColumnDescList(create_tree->GetColumnDefList());
    plan_tree->setTableName(create_tree->GetTableName());
}

int SimplePlanner::CreatePlanTree(
    const NodePointVector &parser_trees, PlanNodeList &plan_trees,
    Status &status) {  // NOLINT (runtime/references)
    if (parser_trees.empty()) {
        status.msg = "fail to create plan tree: parser trees is empty";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return status.code;
    }

    for (auto parser_tree : parser_trees) {
        switch (parser_tree->GetType()) {
            case node::kSelectStmt: {
                PlanNode *select_plan = nullptr;
                CreateSelectStmtPlan(parser_tree, &select_plan, status);
                if (0 != status.code) {
                    return status.code;
                }
                plan_trees.push_back(select_plan);
                break;
            }
            case node::kCreateStmt: {
                PlanNode *plan =
                    node_manager_->MakePlanNode(node::kPlanTypeCreate);
                CreateCreateTablePlan(
                    parser_tree, dynamic_cast<node::CreatePlanNode *>(plan),
                    status);
                if (0 != status.code) {
                    return status.code;
                }
                plan_trees.push_back(plan);
                break;
            }
            case node::kCmdStmt: {
                node::PlanNode *cmd_plan =
                    node_manager_->MakePlanNode(node::kPlanTypeCmd);
                CreateCmdPlan(parser_tree,
                              dynamic_cast<node::CmdPlanNode *>(cmd_plan),
                              status);
                if (0 != status.code) {
                    return status.code;
                }
                plan_trees.push_back(cmd_plan);
                break;
            }
            case node::kInsertStmt: {
                node::PlanNode *insert_plan =
                    node_manager_->MakePlanNode(node::kPlanTypeInsert);
                CreateInsertPlan(
                    parser_tree,
                    dynamic_cast<node::InsertPlanNode *>(insert_plan), status);
                plan_trees.push_back(insert_plan);
                break;
            }
            case ::fesql::node::kFnDef: {
                node::PlanNode *fn_plan =
                    node_manager_->MakePlanNode(node::kPlanTypeFuncDef);
                CreateFuncDefPlan(
                    parser_tree, dynamic_cast<node::FuncDefPlanNode *>(fn_plan),
                    status);
                plan_trees.push_back(fn_plan);
                break;
            }
            default: {
                status.msg = "can not handle tree type " +
                             node::NameOfSQLNodeType(parser_tree->GetType());
                status.code = common::kPlanError;
                LOG(WARNING) << status.msg;
                return status.code;
            }
        }
    }

    return status.code;
}
/***
 * Create function def plan node
 * 1. check indent
 * 2. construct sub blocks
 *      if_then_else block
 *
 * @param root
 * @param plan
 * @param status
 */
void Planner::CreateFuncDefPlan(
    const SQLNode *root, node::FuncDefPlanNode *plan,
    Status &status) {  // NOLINT (runtime/references)
    if (nullptr == root) {
        status.msg =
            "fail to create func def plan node: query tree node it null";
        status.code = common::kSQLError;
        LOG(WARNING) << status.msg;
        return;
    }

    if (root->GetType() != node::kFnDef) {
        status.code = common::kSQLError;
        status.msg =
            "fail to create cmd plan node: query tree node it not function "
            "def "
            "type";
        LOG(WARNING) << status.msg;
        return;
    }
    plan->fn_def_ = dynamic_cast<const node::FnNodeFnDef *>(root);
}

void Planner::CreateInsertPlan(const node::SQLNode *root,
                               node::InsertPlanNode *plan,
                               Status &status) {  // NOLINT (runtime/references)
    if (nullptr == root) {
        status.msg = "fail to create cmd plan node: query tree node it null";
        status.code = common::kSQLError;
        LOG(WARNING) << status.msg;
        return;
    }

    if (root->GetType() != node::kInsertStmt) {
        status.msg =
            "fail to create cmd plan node: query tree node it not insert "
            "type";
        status.code = common::kSQLError;
        return;
    }

    plan->SetInsertNode(dynamic_cast<const node::InsertStmt *>(root));
}

void Planner::CreateCmdPlan(const SQLNode *root, node::CmdPlanNode *plan,
                            Status &status) {  // NOLINT (runtime/references)
    if (nullptr == root) {
        status.msg = "fail to create cmd plan node: query tree node it null";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return;
    }

    if (root->GetType() != node::kCmdStmt) {
        status.msg =
            "fail to create cmd plan node: query tree node it not cmd type";
        status.code = common::kPlanError;
        return;
    }

    plan->SetCmdNode(dynamic_cast<const node::CmdNode *>(root));
}

bool TransformTableDef(const std::string &table_name,
                       const NodePointVector &column_desc_list,
                       type::TableDef *table,
                       Status &status) {  // NOLINT (runtime/references)
    std::set<std::string> index_names;
    std::set<std::string> column_names;

    for (auto column_desc : column_desc_list) {
        switch (column_desc->GetType()) {
            case node::kColumnDesc: {
                node::ColumnDefNode *column_def =
                    (node::ColumnDefNode *)column_desc;
                type::ColumnDef *column = table->add_columns();

                if (column_names.find(column_def->GetColumnName()) !=
                    column_names.end()) {
                    status.msg = "CREATE common: COLUMN NAME " +
                                 column_def->GetColumnName() + " duplicate";
                    status.code = common::kSQLError;
                    LOG(WARNING) << status.msg;
                    return false;
                }
                column->set_name(column_def->GetColumnName());
                column->set_is_not_null(column_def->GetIsNotNull());
                column_names.insert(column_def->GetColumnName());
                fesql::type::Type c_type;
                switch (column_def->GetColumnType()) {
                    case node::kBool:
                        column->set_type(type::Type::kBool);
                        break;
                    case node::kInt32:
                        column->set_type(type::Type::kInt32);
                        break;
                    case node::kInt64:
                        column->set_type(type::Type::kInt64);
                        break;
                    case node::kFloat:
                        column->set_type(type::Type::kFloat);
                        break;
                    case node::kDouble:
                        column->set_type(type::Type::kDouble);
                        break;
                    case node::kTimestamp: {
                        column->set_type(type::Type::kTimestamp);
                        break;
                    }
                    case node::kVarchar:
                        column->set_type(type::Type::kVarchar);
                        break;
                    default: {
                        status.msg =
                            "CREATE common: column type " +
                            node::DataTypeName(column_def->GetColumnType()) +
                            " is not supported";
                        status.code = common::kSQLError;
                        LOG(WARNING) << status.msg;
                        return false;
                    }
                }
                break;
            }

            case node::kColumnIndex: {
                node::ColumnIndexNode *column_index =
                    (node::ColumnIndexNode *)column_desc;

                if (column_index->GetName().empty()) {
                    column_index->SetName(
                        GenerateName("INDEX", table->indexes_size()));
                }
                if (index_names.find(column_index->GetName()) !=
                    index_names.end()) {
                    status.msg = "CREATE common: INDEX NAME " +
                                 column_index->GetName() + " duplicate";
                    status.code = common::kSQLError;
                    LOG(WARNING) << status.msg;
                    return false;
                }
                index_names.insert(column_index->GetName());
                type::IndexDef *index = table->add_indexes();
                index->set_name(column_index->GetName());

                // TODO(chenjing): set ttl per key
                if (-1 != column_index->GetTTL()) {
                    index->add_ttl(column_index->GetTTL());
                }

                for (auto key : column_index->GetKey()) {
                    index->add_first_keys(key);
                }

                if (!column_index->GetTs().empty()) {
                    index->set_second_key(column_index->GetTs());
                }
                break;
            }
            default: {
                status.msg = "can not support " +
                             node::NameOfSQLNodeType(column_desc->GetType()) +
                             " when CREATE TABLE";
                status.code = common::kSQLError;
                LOG(WARNING) << status.msg;
                return false;
            }
        }
    }
    table->set_name(table_name);
    return true;
}

std::string GenerateName(const std::string prefix, int id) {
    time_t t;
    time(&t);
    std::string name =
        prefix + "_" + std::to_string(id) + "_" + std::to_string(t);
    return name;
}

}  // namespace plan
}  // namespace fesql
