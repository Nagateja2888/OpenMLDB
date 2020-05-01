/*-------------------------------------------------------------------------
 * Copyright (C) 2020, 4paradigm
 * transform.cc
 *
 * Author: chenjing
 * Date: 2020/3/13
 *--------------------------------------------------------------------------
 **/
#include "vm/transform.h"
#include <set>
#include <stack>
#include <unordered_map>
#include "codegen/fn_ir_builder.h"
#include "codegen/fn_let_ir_builder.h"
#include "physical_op.h"
#include "vm/physical_op.h"

namespace fesql {
namespace vm {

std::ostream& operator<<(std::ostream& output,
                         const fesql::vm::LogicalOp& thiz) {
    return output << *(thiz.node_);
}
bool TransformLogicalTreeToLogicalGraph(
    const ::fesql::node::PlanNode* node, LogicalGraph* graph_ptr,
    fesql::base::Status& status) {  // NOLINT

    if (nullptr == node || nullptr == graph_ptr) {
        status.msg = "node or graph_ptr is null";
        status.code = common::kOpGenError;
        LOG(WARNING) << status.msg;
        return false;
    }
    auto& graph = *graph_ptr;
    std::stack<LogicalOp> stacks;
    LogicalOp op(node);
    graph.AddVertex(op);
    stacks.push(op);
    while (!stacks.empty()) {
        auto source = stacks.top();
        stacks.pop();
        auto& children = source.node_->GetChildren();
        if (!children.empty()) {
            for (auto iter = children.cbegin(); iter != children.cend();
                 iter++) {
                LogicalOp target(*iter);
                if (!graph.IsExist(target)) {
                    stacks.push(target);
                }
                graph.AddEdge(source, target);
            }
        }
    }
    return true;
}

BatchModeTransformer::BatchModeTransformer(
    node::NodeManager* node_manager, const std::string& db,
    const std::shared_ptr<Catalog>& catalog, ::llvm::Module* module)
    : node_manager_(node_manager),
      db_(db),
      catalog_(catalog),
      module_(module),
      id_(0) {}

BatchModeTransformer::~BatchModeTransformer() {}

bool BatchModeTransformer::TransformPlanOp(const ::fesql::node::PlanNode* node,
                                           ::fesql::vm::PhysicalOpNode** ouput,
                                           ::fesql::base::Status& status) {
    if (nullptr == node || nullptr == ouput) {
        status.msg = "input node or output node is null";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }
    LogicalOp logical_op = LogicalOp(node);
    auto map_iter = op_map_.find(logical_op);
    // logical plan node already exist
    if (map_iter != op_map_.cend()) {
        *ouput = map_iter->second;
        return true;
    }

    ::fesql::vm::PhysicalOpNode* op = nullptr;
    bool ok = true;
    switch (node->type_) {
        case node::kPlanTypeLimit:
            ok = TransformLimitOp(
                dynamic_cast<const ::fesql::node::LimitPlanNode*>(node), &op,
                status);
            break;
        case node::kPlanTypeProject:
            ok = TransformProjecPlantOp(
                dynamic_cast<const ::fesql::node::ProjectPlanNode*>(node), &op,
                status);
            break;
        case node::kPlanTypeJoin:
            ok = TransformJoinOp(
                dynamic_cast<const ::fesql::node::JoinPlanNode*>(node), &op,
                status);
            break;
        case node::kPlanTypeUnion:
            ok = TransformUnionOp(
                dynamic_cast<const ::fesql::node::UnionPlanNode*>(node), &op,
                status);
            break;
        case node::kPlanTypeGroup:
            ok = TransformGroupOp(
                dynamic_cast<const ::fesql::node::GroupPlanNode*>(node), &op,
                status);
            break;
        case node::kPlanTypeSort:
            ok = TransformSortOp(
                dynamic_cast<const ::fesql::node::SortPlanNode*>(node), &op,
                status);
            break;
        case node::kPlanTypeFilter:
            ok = TransformFilterOp(
                dynamic_cast<const ::fesql::node::FilterPlanNode*>(node), &op,
                status);
            break;
        case node::kPlanTypeTable:
            ok = TransformScanOp(
                dynamic_cast<const ::fesql::node::TablePlanNode*>(node), &op,
                status);
            break;
        case node::kPlanTypeQuery:
            ok = TransformQueryPlan(
                dynamic_cast<const ::fesql::node::QueryPlanNode*>(node), &op,
                status);
            break;
        case node::kPlanTypeRename:
            ok = TransformRenameOp(
                dynamic_cast<const ::fesql::node::RenamePlanNode*>(node), &op,
                status);
            break;
        case node::kPlanTypeDistinct:
            ok = TransformDistinctOp(
                dynamic_cast<const ::fesql::node::DistinctPlanNode*>(node), &op,
                status);
            break;
        default: {
            status.msg = "fail to transform physical plan: can't handle type " +
                         node::NameOfPlanNodeType(node->type_);
            status.code = common::kPlanError;
            LOG(WARNING) << status.msg;
            return false;
        }
    }
    if (!ok) {
        LOG(WARNING) << "fail to tranform physical plan: fail node " +
                            node::NameOfPlanNodeType(node->type_);
        return false;
    }
    op_map_[logical_op] = op;
    *ouput = op;
    return true;
}

bool BatchModeTransformer::GenPlanNode(PhysicalOpNode* node,
                                       base::Status& status) {
    if (nullptr == node) {
        status.msg = "input node is null";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }

    for (auto producer : node->producers()) {
        if (!GenPlanNode(producer, status)) {
            return false;
        }
    }

    if (!node->GetFnName().empty()) {
        DLOG(INFO) << "already gen code for node";
        return true;
    }
    std::string fn_name = "";
    vm::Schema fn_schema;
    switch (node->type_) {
        case kPhysicalOpGroupBy: {
            auto group_op = dynamic_cast<PhysicalGroupNode*>(node);
            if (!GenGroup(&group_op->group_, node->producers()[0], status)) {
                return false;
            }
            break;
        }
        case kPhysicalOpSortBy: {
            auto sort_op = dynamic_cast<PhysicalSortNode*>(node);
            if (!GenSort(&sort_op->sort_, node->producers()[0], status)) {
                return false;
            }
            break;
        }
        case kPhysicalOpProject: {
            auto project_op = dynamic_cast<PhysicalProjectNode*>(node);
            if (kWindowAggregation != project_op->project_type_) {
                return false;
            }
            auto window_agg_op =
                dynamic_cast<PhysicalWindowAggrerationNode*>(node);
            if (!GenWindow(&window_agg_op->window_, node->producers()[0],
                           status)) {
                return false;
            }
            break;
        }
        case kPhysicalOpFilter: {
            auto op = dynamic_cast<PhysicalFliterNode*>(node);
            if (!GenFilter(&op->filter_, node->producers()[0], status)) {
                return false;
            }
            break;
        }
        case kPhysicalOpJoin: {
            auto op = dynamic_cast<PhysicalJoinNode*>(node);
            if (!GenJoin(&op->join_, node, status)) {
                return false;
            }

            break;
        }
        case kPhysicalOpRequestJoin: {
            auto request_join_op = dynamic_cast<PhysicalRequestJoinNode*>(node);
            if (!GenJoin(&request_join_op->join_, node, status)) {
                return false;
            }
            break;
        }
        case kPhysicalOpRequestUnoin: {
            auto request_union_op =
                dynamic_cast<PhysicalRequestUnionNode*>(node);
            if (!GenWindow(&request_union_op->window_, node->producers()[1],
                           status)) {
                return false;
            }
            break;
        }
        default: {
            return true;
        }
    }
    node->SetFnSchema(fn_schema);
    node->SetFnName(fn_name);
    return true;
}
bool BatchModeTransformer::TransformLimitOp(const node::LimitPlanNode* node,
                                            PhysicalOpNode** output,
                                            base::Status& status) {
    if (nullptr == node || nullptr == output) {
        status.msg = "input node or output node is null";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }
    PhysicalOpNode* depend = nullptr;
    if (!TransformPlanOp(node->GetChildren()[0], &depend, status)) {
        return false;
    }
    *output = new PhysicalLimitNode(depend, node->limit_cnt_);
    node_manager_->RegisterNode(*output);
    return true;
}

bool BatchModeTransformer::TransformProjecPlantOp(
    const node::ProjectPlanNode* node, PhysicalOpNode** output,
    base::Status& status) {
    if (nullptr == node || nullptr == output) {
        status.msg = "input node or output node is null";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }
    PhysicalOpNode* depend = nullptr;
    if (!TransformPlanOp(node->GetChildren()[0], &depend, status)) {
        return false;
    }

    if (node->project_list_vec_.empty()) {
        status.msg = "fail transform project op: empty projects";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }

    if (1 == node->project_list_vec_.size()) {
        return TransformProjectOp(dynamic_cast<fesql::node::ProjectListNode*>(
                                      node->project_list_vec_[0]),
                                  depend, output, status);
    }

    std::vector<PhysicalOpNode*> ops;
    int32_t project_cnt = 0;
    for (size_t i = node->project_list_vec_.size() - 1; i > 0; i--) {
        fesql::node::ProjectListNode* project_list =
            dynamic_cast<fesql::node::ProjectListNode*>(
                node->project_list_vec_[i]);
        project_cnt++;
        // append oringinal table column after project columns
        // if there is multi
        project_list->AddProject(node_manager_->MakeRowProjectNode(
            project_list->GetProjects().size(), "*",
            node_manager_->MakeAllNode("")));

        PhysicalOpNode* project_op = nullptr;
        if (!TransformProjectOp(project_list, depend, &project_op, status)) {
            return false;
        }
        depend = project_op;
    }

    // 第一个Project节点除了计算投影表达式之外，还需要筛选出前面窗口的表达式结果
    // TODO(chenjing): 这部分代码可读性还是太差
    fesql::node::ProjectListNode* first_project_list =
        dynamic_cast<fesql::node::ProjectListNode*>(node->project_list_vec_[0]);
    auto project_list = node_manager_->MakeProjectListPlanNode(
        first_project_list->w_ptr_, first_project_list->is_window_agg_);
    uint32_t pos = 0;
    for (auto iter = node->pos_mapping_.cbegin();
         iter != node->pos_mapping_.cend(); iter++) {
        auto sub_project_list = dynamic_cast<node::ProjectListNode*>(
            node->project_list_vec_[iter->first]);

        auto project_node = dynamic_cast<node::ProjectNode*>(
            sub_project_list->GetProjects().at(iter->second));
        if (iter->first == 0) {
            project_list->AddProject(project_node);

        } else if (node::kExprAll ==
                   project_node->GetExpression()->expr_type_) {
            auto all_expr =
                dynamic_cast<node::AllNode*>(project_node->GetExpression());
            if (all_expr->children_.empty()) {
                // expand all expression if needed
                for (auto column : depend->output_schema_) {
                    all_expr->children_.push_back(
                        node_manager_->MakeColumnRefNode(
                            column.name(), all_expr->GetRelationName()));
                }
            }
            project_list->AddProject(
                node_manager_->MakeRowProjectNode(pos, "*", all_expr));
        } else {
            project_list->AddProject(node_manager_->MakeRowProjectNode(
                pos, project_node->GetName(),
                node_manager_->MakeColumnRefNode(project_node->GetName(), "")));
        }
        pos++;
    }
    return TransformProjectOp(project_list, depend, output, status);
}

bool BatchModeTransformer::TransformWindowOp(PhysicalOpNode* depend,
                                             const node::WindowPlanNode* w_ptr,
                                             PhysicalOpNode** output,
                                             base::Status& status) {
    if (nullptr == depend || nullptr == w_ptr || nullptr == output) {
        status.msg = "depend node or output node is null";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }
    const node::OrderByNode* orders = w_ptr->GetOrders();
    const node::ExprListNode* groups = w_ptr->GetKeys();

    switch (depend->type_) {
        case kPhysicalOpDataProvider: {
            auto data_op = dynamic_cast<PhysicalDataProviderNode*>(depend);
            if (kProviderTypeRequest == data_op->provider_type_) {
                auto name = data_op->table_handler_->GetName();
                auto table = catalog_->GetTable(db_, name);
                if (table) {
                    auto right = new PhysicalTableProviderNode(table);
                    node_manager_->RegisterNode(right);
                    *output = new PhysicalRequestUnionNode(
                        depend, right, groups, orders, w_ptr->GetStartOffset(),
                        w_ptr->GetEndOffset());
                    node_manager_->RegisterNode(*output);
                    return true;
                } else {
                    status.code = common::kPlanError;
                    status.msg = "fail to transform data provider op: table " +
                                 name + "not exists";
                    LOG(WARNING) << status.msg;
                    return false;
                }
            }
            break;
        }
        case kPhysicalOpRequestJoin: {
            auto join_op = dynamic_cast<PhysicalRequestJoinNode*>(depend);
            switch (join_op->join_type_) {
                case node::kJoinTypeLeft:
                case node::kJoinTypeLast: {
                    SchemasContext ctx(depend->GetOutputNameSchemaList());
                    if (!node::ExprListNullOrEmpty(groups)) {
                        const RowSchemaInfo* info;
                        if (!ctx.ExprListResolved(groups->children_, &info)) {
                            status.msg =
                                "fail to handle window: group "
                                "expression should belong to left table";
                            LOG(WARNING) << status.msg;
                            return false;
                        }
                        if (0 != info->idx_) {
                            status.msg =
                                "fail to handle window: group "
                                "expression should belong to left table";
                            LOG(WARNING) << status.msg;
                            return false;
                        }
                    }
                    if (nullptr != orders &&
                        !node::ExprListNullOrEmpty(orders->order_by_)) {
                        const RowSchemaInfo* info;
                        if (!ctx.ExprListResolved(orders->order_by_->children_,
                                                  &info)) {
                            status.msg =
                                "fail to handle window: order "
                                "expression should belong to left table";
                            LOG(WARNING) << status.msg;
                            return false;
                        }
                        if (0 != info->idx_) {
                            status.msg =
                                "fail to handle window: order "
                                "expression should belong to left table";
                            LOG(WARNING) << status.msg;
                            return false;
                        }
                    }

                    auto request_op = dynamic_cast<PhysicalDataProviderNode*>(
                        join_op->producers()[0]);
                    auto name = request_op->table_handler_->GetName();
                    auto table = catalog_->GetTable(db_, name);
                    if (table) {
                        auto right = new PhysicalTableProviderNode(table);
                        node_manager_->RegisterNode(right);
                        auto request_union_op = new PhysicalRequestUnionNode(
                            request_op, right, groups, orders,
                            w_ptr->GetStartOffset(), w_ptr->GetEndOffset());
                        node_manager_->RegisterNode(request_union_op);
                        *output = new PhysicalJoinNode(
                            request_union_op, join_op->producers()[1],
                            join_op->join_type_, join_op->join_);
                        return true;
                    } else {
                        status.code = common::kPlanError;
                        status.msg =
                            "fail to transform data provider op: table " +
                            name + "not exists";
                        LOG(WARNING) << status.msg;
                        return false;
                    }
                    break;
                }
                default: {
                    // do nothing
                }
            }
        }
        default: {
            // do nothing
        }
    }
    return true;
}
bool BatchModeTransformer::TransformJoinOp(const node::JoinPlanNode* node,
                                           PhysicalOpNode** output,
                                           base::Status& status) {
    if (nullptr == node || nullptr == output) {
        status.msg = "input node or output node is null";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }
    PhysicalOpNode* left = nullptr;
    PhysicalOpNode* right = nullptr;
    if (!TransformPlanOp(node->GetChildren()[0], &left, status)) {
        return false;
    }
    if (!TransformPlanOp(node->GetChildren()[1], &right, status)) {
        return false;
    }
    *output =
        new PhysicalJoinNode(left, right, node->join_type_, node->condition_);
    node_manager_->RegisterNode(*output);
    return true;
}
bool BatchModeTransformer::TransformUnionOp(const node::UnionPlanNode* node,
                                            PhysicalOpNode** output,
                                            base::Status& status) {
    if (nullptr == node || nullptr == output) {
        status.msg = "input node or output node is null";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }
    PhysicalOpNode* left = nullptr;
    PhysicalOpNode* right = nullptr;
    if (!TransformPlanOp(node->GetChildren()[0], &left, status)) {
        return false;
    }
    if (!TransformPlanOp(node->GetChildren()[1], &right, status)) {
        return false;
    }
    *output = new PhysicalUnionNode(left, right, node->is_all);
    node_manager_->RegisterNode(*output);
    return true;
}
bool BatchModeTransformer::TransformGroupOp(const node::GroupPlanNode* node,
                                            PhysicalOpNode** output,
                                            base::Status& status) {
    PhysicalOpNode* left = nullptr;
    if (!TransformPlanOp(node->GetChildren()[0], &left, status)) {
        return false;
    }

    if (kPhysicalOpDataProvider == left->type_) {
        auto data_op = dynamic_cast<PhysicalDataProviderNode*>(left);
        if (kProviderTypeRequest == data_op->provider_type_) {
            auto name = data_op->table_handler_->GetName();
            auto table = catalog_->GetTable(db_, name);
            if (table) {
                auto right = new PhysicalTableProviderNode(table);
                node_manager_->RegisterNode(right);
                *output = new PhysicalRequestUnionNode(
                    left, right, node->by_list_, nullptr, -1, -1);
                node_manager_->RegisterNode(*output);
                return true;
            } else {
                status.code = common::kPlanError;
                status.msg = "fail to transform data provider op: table " +
                             name + "not exists";
                LOG(WARNING) << status.msg;
                return false;
            }
        }
    }

    *output = new PhysicalGroupNode(left, node->by_list_);
    node_manager_->RegisterNode(*output);
    return true;
}
bool BatchModeTransformer::TransformSortOp(const node::SortPlanNode* node,
                                           PhysicalOpNode** output,
                                           base::Status& status) {
    if (nullptr == node || nullptr == output) {
        status.msg = "input node or output node is null";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }
    PhysicalOpNode* left = nullptr;
    if (!TransformPlanOp(node->GetChildren()[0], &left, status)) {
        return false;
    }
    *output = new PhysicalSortNode(left, node->order_list_);
    node_manager_->RegisterNode(*output);
    return true;
}
bool BatchModeTransformer::TransformFilterOp(const node::FilterPlanNode* node,
                                             PhysicalOpNode** output,
                                             base::Status& status) {
    if (nullptr == node || nullptr == output) {
        status.msg = "input node or output node is null";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }
    PhysicalOpNode* depend = nullptr;
    if (!TransformPlanOp(node->GetChildren()[0], &depend, status)) {
        return false;
    }
    *output = new PhysicalFliterNode(depend, node->condition_);
    node_manager_->RegisterNode(*output);
    return true;
}

bool BatchModeTransformer::TransformScanOp(const node::TablePlanNode* node,
                                           PhysicalOpNode** output,
                                           base::Status& status) {
    if (nullptr == node || nullptr == output) {
        status.msg = "input node or output node is null";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }
    auto table = catalog_->GetTable(db_, node->table_);
    if (table) {
        *output = new PhysicalTableProviderNode(table);
        node_manager_->RegisterNode(*output);
    } else {
        status.msg = "fail to transform data_provider op: table " + db_ + "." +
                     node->table_ + " not exist!";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }
    return true;
}

bool BatchModeTransformer::TransformRenameOp(const node::RenamePlanNode* node,
                                             PhysicalOpNode** output,
                                             base::Status& status) {
    if (nullptr == node || nullptr == output) {
        status.msg = "input node or output node is null";
        status.code = common::kPlanError;
        return false;
    }
    PhysicalOpNode* left = nullptr;
    if (!TransformPlanOp(node->GetChildren()[0], &left, status)) {
        return false;
    }
    *output = new PhysicalRenameNode(left, node->table_);
    node_manager_->RegisterNode(*output);
    return true;
}

bool BatchModeTransformer::TransformDistinctOp(
    const node::DistinctPlanNode* node, PhysicalOpNode** output,
    base::Status& status) {
    if (nullptr == node || nullptr == output) {
        status.msg = "input node or output node is null";
        status.code = common::kPlanError;
        return false;
    }
    PhysicalOpNode* left = nullptr;
    if (!TransformPlanOp(node->GetChildren()[0], &left, status)) {
        return false;
    }
    *output = new PhysicalDistinctNode(left);
    node_manager_->RegisterNode(*output);
    return true;
}
bool BatchModeTransformer::TransformQueryPlan(
    const ::fesql::node::PlanNode* node, ::fesql::vm::PhysicalOpNode** output,
    ::fesql::base::Status& status) {
    if (nullptr == node || nullptr == output) {
        status.msg = "input node or output node is null";
        status.code = common::kPlanError;
        return false;
    }
    if (!TransformPlanOp(node->GetChildren()[0], output, status)) {
        return false;
    }
    return true;
}
bool BatchModeTransformer::AddPass(PhysicalPlanPassType type) {
    passes.push_back(type);
    return true;
}
bool BatchModeTransformer::GenProjects(
    const std::vector<std::pair<const std::string, const Schema*>>&
        input_name_schema_list,
    const node::PlanNodeList& projects, const bool row_project,
    std::string& fn_name,    // NOLINT
    Schema* output_schema,   // NOLINT
    base::Status& status) {  // NOLINT
    // TODO(wangtaize) use ops end op output schema
    ::fesql::codegen::RowFnLetIRBuilder builder(input_name_schema_list,
                                                module_);
    fn_name = "__internal_sql_codegen_" + std::to_string(id_++);
    bool ok = builder.Build(fn_name, projects, output_schema);
    if (!ok) {
        status.code = common::kCodegenError;
        status.msg = "fail to codegen projects node";
        LOG(WARNING) << status.msg;
        return false;
    }
    return true;
}
bool BatchModeTransformer::AddDefaultPasses() {
    AddPass(kPassFilterOptimized);
    AddPass(kPassLeftJoinOptimized);
    AddPass(kPassGroupAndSortOptimized);
    AddPass(kPassLimitOptimized);
    return false;
}

bool BatchModeTransformer::CreatePhysicalProjectNode(
    const ProjectType project_type, PhysicalOpNode* node,
    node::ProjectListNode* project_list, PhysicalOpNode** output,
    base::Status& status) {
    if (nullptr == project_list || nullptr == output) {
        status.msg = "project node or output node is null";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }
    const node::PlanNodeList& projects = project_list->GetProjects();

    node::PlanNodeList new_projects;
    bool has_all_project = false;

    for (auto iter = projects.cbegin(); iter != projects.cend(); iter++) {
        auto project_node = dynamic_cast<node::ProjectNode*>(*iter);
        auto expr = project_node->GetExpression();
        if (nullptr == expr) {
            status.msg = "invalid project: expression is null";
            status.code = common::kPlanError;
            return false;
        }
        if (node::kExprAll == expr->expr_type_) {
            auto all_expr = dynamic_cast<node::AllNode*>(expr);
            if (all_expr->children_.empty()) {
                // expand all expression if needed
                for (auto column : node->output_schema_) {
                    all_expr->children_.push_back(
                        node_manager_->MakeColumnRefNode(
                            column.name(), all_expr->GetRelationName()));
                }
            }
            has_all_project = true;
        }
    }

    if (has_all_project && 1 == projects.size()) {
        // skip project
        DLOG(INFO) << "skip project node: project has only kAllExpr "
                      "expression";
        *output = node;
        return true;
    }

    Schema output_schema;
    std::string fn_name;
    switch (project_type) {
        case kRowProject:
        case kTableProject: {
            if (!GenProjects((node->GetOutputNameSchemaList()), projects, true,
                             fn_name, &output_schema, status)) {
                return false;
            }
            break;
        }
        case kAggregation:
        case kGroupAggregation:
        case kWindowAggregation: {
            // TODO(chenjing): gen window aggregation
            if (!GenProjects((node->GetOutputNameSchemaList()), projects, false,
                             fn_name, &output_schema, status)) {
                return false;
            }
            break;
        }
    }

    PhysicalOpNode* op = nullptr;
    switch (project_type) {
        case kRowProject: {
            op = new PhysicalRowProjectNode(node, fn_name, output_schema);
            break;
        }
        case kTableProject: {
            op = new PhysicalTableProjectNode(node, fn_name, output_schema);
            break;
        }
        case kAggregation: {
            op = new PhysicalAggrerationNode(node, fn_name, output_schema);
            break;
        }
        case kGroupAggregation: {
            auto group_op = dynamic_cast<PhysicalGroupNode*>(node);
            op = new PhysicalGroupAggrerationNode(
                node, group_op->group_.groups(), fn_name, output_schema);
            break;
        }
        case kWindowAggregation: {
            op = new PhysicalWindowAggrerationNode(
                node, project_list->w_ptr_->GetKeys(),
                project_list->w_ptr_->GetOrders(), fn_name, output_schema,
                project_list->w_ptr_->GetStartOffset(),
                project_list->w_ptr_->GetEndOffset());
            break;
        }
    }
    node_manager_->RegisterNode(op);
    *output = op;
    return true;
}  // namespace vm

bool BatchModeTransformer::TransformProjectOp(
    node::ProjectListNode* project_list, PhysicalOpNode* node,
    PhysicalOpNode** output, base::Status& status) {
    auto depend = node;
    switch (depend->output_type_) {
        case kSchemaTypeRow:
            return CreatePhysicalProjectNode(kRowProject, depend, project_list,
                                             output, status);
        case kSchemaTypeGroup:
            return CreatePhysicalProjectNode(kGroupAggregation, depend,
                                             project_list, output, status);
        case kSchemaTypeTable:
            if (project_list->is_window_agg_) {
                return CreatePhysicalProjectNode(kWindowAggregation, depend,
                                                 project_list, output, status);
            } else {
                return CreatePhysicalProjectNode(kTableProject, depend,
                                                 project_list, output, status);
            }
    }
    return false;
}
void BatchModeTransformer::ApplyPasses(PhysicalOpNode* node,
                                       PhysicalOpNode** output) {
    auto physical_plan = node;
    for (auto type : passes) {
        switch (type) {
            case kPassFilterOptimized: {
                ConditionOptimized pass(node_manager_, db_, catalog_);
                PhysicalOpNode* new_op = nullptr;
                if (pass.Apply(physical_plan, &new_op)) {
                    physical_plan = new_op;
                }
                break;
            }
            case kPassGroupAndSortOptimized: {
                GroupAndSortOptimized pass(node_manager_, db_, catalog_);
                PhysicalOpNode* new_op = nullptr;
                if (pass.Apply(physical_plan, &new_op)) {
                    physical_plan = new_op;
                }
                break;
            }
            case kPassLeftJoinOptimized: {
                LeftJoinOptimized pass(node_manager_, db_, catalog_);
                PhysicalOpNode* new_op = nullptr;
                if (pass.Apply(physical_plan, &new_op)) {
                    physical_plan = new_op;
                }
                break;
            }
            case kPassLimitOptimized: {
                LimitOptimized pass(node_manager_, db_, catalog_);
                PhysicalOpNode* new_op = nullptr;
                if (pass.Apply(physical_plan, &new_op)) {
                    physical_plan = new_op;
                }
                break;
            }
            default: {
                LOG(WARNING) << "can't not handle pass: "
                             << PhysicalPlanPassTypeName(type);
            }
        }
    }
    *output = physical_plan;
}
bool BatchModeTransformer::TransformPhysicalPlan(
    const ::fesql::node::PlanNodeList& trees,
    ::fesql::vm::PhysicalOpNode** output, base::Status& status) {
    if (nullptr == module_ || trees.empty()) {
        status.msg = "module or logical trees is empty";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }

    auto it = trees.begin();
    for (; it != trees.end(); ++it) {
        const ::fesql::node::PlanNode* node = *it;
        switch (node->GetType()) {
            case ::fesql::node::kPlanTypeFuncDef: {
                const ::fesql::node::FuncDefPlanNode* func_def_plan =
                    dynamic_cast<const ::fesql::node::FuncDefPlanNode*>(node);
                if (!GenFnDef(func_def_plan, status)) {
                    return false;
                }
                break;
            }
            case ::fesql::node::kPlanTypeUnion:
            case ::fesql::node::kPlanTypeQuery: {
                PhysicalOpNode* physical_plan = nullptr;
                if (!TransformQueryPlan(node, &physical_plan, status)) {
                    LOG(WARNING)
                        << "fail to transform query plan to physical plan";
                    return false;
                }
                ApplyPasses(physical_plan, output);
                if (!GenPlanNode(*output, status)) {
                    LOG(WARNING) << "fail to gen plan";
                    return false;
                }
                break;
            }
            default: {
                LOG(WARNING) << "not supported";
                return false;
            }
        }
    }
    return true;
}
bool BatchModeTransformer::GenFnDef(const node::FuncDefPlanNode* fn_plan,
                                    base::Status& status) {
    if (nullptr == module_ || nullptr == fn_plan ||
        nullptr == fn_plan->fn_def_) {
        status.msg = "fail to codegen function: module or fn_def node is null";
        status.code = common::kOpGenError;
        LOG(WARNING) << status.msg;
        return false;
    }

    ::fesql::codegen::FnIRBuilder builder(module_);
    bool ok = builder.Build(fn_plan->fn_def_, status);
    if (!ok) {
        status.msg = "fail to codegen function: " + status.msg;
        status.code = common::kCodegenError;
        LOG(WARNING) << status.msg;
        return false;
    }
    return true;
}

bool BatchModeTransformer::CodeGenExprList(
    const NameSchemaList& input_name_schema_list,
    const node::ExprListNode* expr_list, bool row_mode, FnInfo* fn_info,
    base::Status& status) {
    if (node::ExprListNullOrEmpty(expr_list)) {
        status.msg = "fail to codegen expr list: null or empty list";
        status.code = common::kCodegenError;
        return false;
    }
    node::PlanNodeList projects;
    int32_t pos = 0;
    for (auto expr : expr_list->children_) {
        projects.push_back(node_manager_->MakeRowProjectNode(
            pos++, node::ExprString(expr), expr));
    }
    return GenProjects(input_name_schema_list, projects, true,
                       fn_info->fn_name_, &fn_info->fn_schema_, status);
}
bool BatchModeTransformer::CodeGenExprList(
    const NameSchemaList& input_name_schema_list,
    const node::ExprListNode* expr_list, bool row_mode, std::string& fn_name,
    Schema* output_schema, base::Status& status) {
    if (node::ExprListNullOrEmpty(expr_list)) {
        status.msg = "fail to codegen expr list: null or empty list";
        status.code = common::kCodegenError;
        return false;
    }
    node::PlanNodeList projects;
    int32_t pos = 0;
    for (auto expr : expr_list->children_) {
        projects.push_back(node_manager_->MakeRowProjectNode(
            pos++, node::ExprString(expr), expr));
    }

    return GenProjects(input_name_schema_list, projects, true, fn_name,
                       output_schema, status);
}
bool BatchModeTransformer::GenJoin(Join* join, PhysicalOpNode* in,
                                   base::Status& status) {
    auto filter = join->filter_;
    if (!GenFilter(&join->filter_, in, status)) {
        return false;
    }
    if (!GenHash(&join->left_hash_, in->producers()[0], status)) {
        return false;
    }
    return true;
}
bool BatchModeTransformer::GenFilter(ConditionFilter* filter,
                                     PhysicalOpNode* in, base::Status& status) {
    if (nullptr != filter->condition_) {
        node::ExprListNode expr_list;
        expr_list.AddChild(const_cast<node::ExprNode*>(filter->condition_));
        if (!CodeGenExprList(in->GetOutputNameSchemaList(), &expr_list, true,
                             &filter->fn_info_, status)) {
            return false;
        }
        filter->SetConditionIdxs({0});
    }
    return true;
}
bool BatchModeTransformer::GenHash(Hash* hash, PhysicalOpNode*& in,
                                   base::Status& status) {
    // Gen left key function
    node::ExprListNode expr_list;
    std::vector<int32_t> keys_idxs;
    int32_t idx = 0;
    if (!node::ExprListNullOrEmpty(hash->keys_)) {
        for (auto expr : hash->keys_->children_) {
            expr_list.AddChild(expr);
            keys_idxs.push_back(idx++);
        }
    }
    if (!expr_list.children_.empty()) {
        // Gen left table key
        FnInfo left_key_fn_info;
        if (!CodeGenExprList(in->GetOutputNameSchemaList(), &expr_list, true,
                             &hash->fn_info_, status)) {
            return false;
        }
        hash->SetKeysIdxs(keys_idxs);
    }
    return true;
}
bool BatchModeTransformer::GenWindow(WindowOp* window, PhysicalOpNode* in,
                                     base::Status& status) {
    node::ExprListNode expr_list;
    if (!GenGroup(&window->group_, in, status)) {
        return false;
    }

    if (!GenSort(&window->sort_, in, status)) {
        return false;
    }

    if (!GenHash(&window->hash_, in, status)) {
        return false;
    }
    return true;
}
bool BatchModeTransformer::GenGroup(Group* group, PhysicalOpNode* in,
                                    base::Status& status) {
    node::ExprListNode expr_list;
    if (!node::ExprListNullOrEmpty(group->groups_)) {
        int32_t idx = 0;
        std::vector<int32_t> idxs;
        for (auto expr : group->groups_->children_) {
            expr_list.AddChild(expr);
            idxs.push_back(idx++);
        }
        if (!CodeGenExprList(in->GetOutputNameSchemaList(), &expr_list, true,
                             &group->fn_info_, status)) {
            return false;
        }
        group->SetGroupsIdxs(idxs);
    }
    return true;
}
bool BatchModeTransformer::GenSort(Sort* sort, PhysicalOpNode* in,
                                   base::Status& status) {
    if (nullptr != sort->orders_ &&
        !node::ExprListNullOrEmpty(sort->orders_->order_by_)) {
        node::ExprListNode expr_list;
        std::vector<int32_t> idxs;
        int32_t idx = 0;
        for (auto expr : sort->orders_->order_by_->children_) {
            expr_list.AddChild(expr);
            idxs.push_back(idx++);
        }
        if (!CodeGenExprList(in->GetOutputNameSchemaList(), &expr_list, true,
                             &sort->fn_info_, status)) {
            return false;
        }
        sort->SetOrdersIdxs(idxs);
    }
    return true;
}

bool GroupAndSortOptimized::KeysFilterOptimized(PhysicalOpNode* in,
                                                Group* group, Hash* hash,
                                                PhysicalOpNode** new_in) {
    if (nullptr == group || nullptr == hash) {
        return false;
    }
    if (kPhysicalOpDataProvider == in->type_) {
        auto scan_op = dynamic_cast<PhysicalDataProviderNode*>(in);
        if (kProviderTypeTable == scan_op->provider_type_) {
            std::string index_name;
            const node::ExprListNode* new_groups = nullptr;
            const node::ExprListNode* keys = nullptr;
            auto& index_hint = scan_op->table_handler_->GetIndex();
            if (!TransformGroupExpr(group->groups(), index_hint, &index_name,
                                    &keys, &new_groups)) {
                return false;
            }
            PhysicalPartitionProviderNode* scan_index_op =
                new PhysicalPartitionProviderNode(scan_op->table_handler_,
                                                  index_name);
            group->set_groups(new_groups);
            hash->set_keys(keys);
            *new_in = scan_index_op;
            return true;
        }
    }
    return false;
}
bool GroupAndSortOptimized::GroupOptimized(PhysicalOpNode* in, Group* group,
                                           PhysicalOpNode** new_in) {
    if (nullptr == group) {
        return false;
    }
    if (kPhysicalOpDataProvider == in->type_) {
        auto scan_op = dynamic_cast<PhysicalDataProviderNode*>(in);
        if (kProviderTypeTable == scan_op->provider_type_) {
            const node::ExprListNode* new_groups = nullptr;
            const node::ExprListNode* keys = nullptr;
            const node::OrderByNode* new_orders = nullptr;
            std::string index_name;
            auto& index_hint = scan_op->table_handler_->GetIndex();
            if (!TransformGroupExpr(group->groups(), index_hint, &index_name,
                                    &keys, &new_groups)) {
                return false;
            }
            PhysicalPartitionProviderNode* partition_op =
                new PhysicalPartitionProviderNode(scan_op->table_handler_,
                                                  index_name);
            node_manager_->RegisterNode(partition_op);
            *new_in = partition_op;
            group->set_groups(new_groups);
            return true;
        }
    }
    return false;
}

bool GroupAndSortOptimized::SortOptimized(PhysicalOpNode* in, Sort* sort) {
    if (nullptr == sort) {
        return false;
    }
    if (kPhysicalOpDataProvider == in->type_) {
        auto scan_op = dynamic_cast<PhysicalDataProviderNode*>(in);
        if (kProviderTypePartition != scan_op->provider_type_) {
            return false;
        }
        auto partition_provider =
            dynamic_cast<PhysicalPartitionProviderNode*>(scan_op);
        const node::OrderByNode* new_orders = nullptr;

        auto& index_hint = partition_provider->table_handler_->GetIndex();
        std::string index_name = partition_provider->index_name_;
        auto index_st = index_hint.at(index_name);
        TransformOrderExpr(sort->orders(),
                           *(scan_op->table_handler_->GetSchema()), index_st,
                           &new_orders);
        sort->set_orders(new_orders);
        return true;
    }
    return false;
}

bool GroupAndSortOptimized::Transform(PhysicalOpNode* in,
                                      PhysicalOpNode** output) {
    *output = in;
    switch (in->type_) {
        case kPhysicalOpGroupBy: {
            PhysicalGroupNode* group_op = dynamic_cast<PhysicalGroupNode*>(in);
            PhysicalOpNode* new_producer;
            if (!GroupOptimized(group_op->GetProducer(0),
                                &group_op->group_,
                                &new_producer)) {
                return false;
            }
            group_op->SetProducer(0, new_producer);
            if (!group_op->Valid()) {
                *output = group_op->producers()[0];
            }
            return true;
        }
        case kPhysicalOpProject: {
            auto project_op = dynamic_cast<PhysicalProjectNode*>(in);
            if (kWindowAggregation == project_op->project_type_) {
                PhysicalWindowAggrerationNode* union_op =
                    dynamic_cast<PhysicalWindowAggrerationNode*>(project_op);
                PhysicalOpNode* new_producer;
                if (!GroupOptimized(union_op->GetProducer(0),
                                    &union_op->window_.group_,
                                    &new_producer)) {
                    return false;
                }
                union_op->SetProducer(0, new_producer);
                SortOptimized(union_op->GetProducer(0),
                              &union_op->window_.sort_);
                return true;
            } else {
                return false;
            }
        }
        case kPhysicalOpRequestUnoin: {
            PhysicalRequestUnionNode* union_op =
                dynamic_cast<PhysicalRequestUnionNode*>(in);
            PhysicalOpNode* new_producer;
            if (!KeysFilterOptimized(
                    union_op->GetProducer(1), &union_op->window_.group_,
                    &union_op->window_.hash_, &new_producer)) {
                return false;
            }
            union_op->SetProducer(1, new_producer);
            SortOptimized(union_op->GetProducer(1),
                          &union_op->window_.sort_);
            return true;
        }
        case kPhysicalOpRequestJoin: {
            PhysicalRequestJoinNode* join_op =
                dynamic_cast<PhysicalRequestJoinNode*>(in);
            PhysicalOpNode* new_producer;
            // Optimized Right Table Partition
            if (!GroupOptimized(join_op->GetProducer(1),
                                (&join_op->join_.right_partition_),
                                &new_producer)) {
                return false;
            }
            join_op->SetProducer(1, new_producer);
            return true;
        }
        case kPhysicalOpJoin: {
            PhysicalJoinNode* join_op = dynamic_cast<PhysicalJoinNode*>(in);
            PhysicalOpNode* new_producer;
            // Optimized Right Table Partition
            if (!GroupOptimized(join_op->GetProducer(1),
                                (&join_op->join_.right_partition_),
                                &new_producer)) {
                return false;
            }
            join_op->SetProducer(1, new_producer);
            return true;
        }
        default: {
            return false;
        }
    }
    return false;
}
bool GroupAndSortOptimized::TransformGroupExpr(
    const node::ExprListNode* groups, const IndexHint& index_hint,
    std::string* index_name, const node::ExprListNode** keys_expr,
    const node::ExprListNode** output) {
    if (nullptr == groups || nullptr == output || nullptr == index_name) {
        LOG(WARNING) << "fail to transform group expr : group exor or output "
                        "or index_name ptr is null";
        return false;
    }
    std::vector<std::string> columns;
    for (auto group : groups->children_) {
        switch (group->expr_type_) {
            case node::kExprColumnRef:
                columns.push_back(
                    dynamic_cast<node::ColumnRefNode*>(group)->GetColumnName());
                break;
            default: {
                break;
            }
        }
    }

    if (columns.empty()) {
        return false;
    }

    std::vector<bool> bitmap(columns.size(), true);
    if (MatchBestIndex(columns, index_hint, &bitmap, index_name)) {
        IndexSt index = index_hint.at(*index_name);
        node::ExprListNode* new_groups = node_manager_->MakeExprList();
        std::set<std::string> keys;
        for (auto iter = index.keys.cbegin(); iter != index.keys.cend();
             iter++) {
            keys.insert(iter->name);
        }

        // generate index keys expr
        auto key_expr_list = node_manager_->MakeExprList();
        for (auto iter = index.keys.cbegin(); iter != index.keys.cend();
             iter++) {
            for (auto group : groups->children_) {
                switch (group->expr_type_) {
                    case node::kExprColumnRef: {
                        std::string column =
                            dynamic_cast<node::ColumnRefNode*>(group)
                                ->GetColumnName();
                        // skip group when match index keys
                        if (column == iter->name) {
                            key_expr_list->AddChild(group);
                            break;
                        }
                        break;
                    }
                    default: {
                        new_groups->AddChild(group);
                    }
                }
            }
        }

        // generate new groups expr
        for (auto group : groups->children_) {
            switch (group->expr_type_) {
                case node::kExprColumnRef: {
                    std::string column =
                        dynamic_cast<node::ColumnRefNode*>(group)
                            ->GetColumnName();
                    // skip group when match index keys
                    if (keys.find(column) == keys.cend()) {
                        new_groups->AddChild(group);
                    }
                    break;
                }
                default: {
                    new_groups->AddChild(group);
                }
            }
        }
        *keys_expr = key_expr_list;
        *output = new_groups;
        return true;
    } else {
        return false;
    }
}

bool GroupAndSortOptimized::MatchBestIndex(
    const std::vector<std::string>& columns, const IndexHint& index_hint,
    std::vector<bool>* bitmap_ptr, std::string* index_name) {
    if (nullptr == bitmap_ptr || nullptr == index_name) {
        LOG(WARNING)
            << "fail to match best index: bitmap or index_name ptr is null";
        return false;
    }
    std::set<std::string> column_set;
    auto& bitmap = *bitmap_ptr;
    for (size_t i = 0; i < columns.size(); ++i) {
        if (bitmap[i]) {
            column_set.insert(columns[i]);
        }
    }

    for (auto iter = index_hint.cbegin(); iter != index_hint.cend(); iter++) {
        IndexSt index = iter->second;
        std::set<std::string> keys;
        for (auto key_iter = index.keys.cbegin(); key_iter != index.keys.cend();
             key_iter++) {
            keys.insert(key_iter->name);
        }

        if (column_set == keys) {
            *index_name = index.name;
            return true;
        }
    }

    std::string best_index_name;
    bool succ = false;
    for (size_t i = 0; i < bitmap.size(); ++i) {
        if (bitmap[i]) {
            bitmap[i] = false;
            std::string name;
            if (MatchBestIndex(columns, index_hint, bitmap_ptr, &name)) {
                succ = true;
                if (best_index_name.empty()) {
                    best_index_name = name;
                } else {
                    auto org_index = index_hint.at(best_index_name);
                    auto new_index = index_hint.at(name);
                    if (org_index.keys.size() < new_index.keys.size()) {
                        best_index_name = name;
                    }
                }
            }
            bitmap[i] = true;
        }
    }
    *index_name = best_index_name;
    return succ;
}

bool ConditionOptimized::JoinConditionOptimized(PhysicalOpNode* in,
                                                Join* join) {
    node::ExprListNode and_conditions;
    if (!TransfromAndConditionList(join->filter_.condition_, &and_conditions)) {
        return false;
    }

    node::ExprListNode new_and_conditions;
    std::vector<ExprPair> condition_eq_pair;
    if (!TransformEqualExprPair(in->GetOutputNameSchemaList(), &and_conditions,
                                &new_and_conditions, condition_eq_pair)) {
        return false;
    }
    node::ExprListNode* left_keys = node_manager_->MakeExprList();
    node::ExprListNode* right_keys = node_manager_->MakeExprList();
    for (auto pair : condition_eq_pair) {
        right_keys->AddChild(pair.right_expr_);
        left_keys->AddChild(pair.left_expr_);
    }
    node::ExprNode* filter_condition =
        node_manager_->MakeAndExpr(&new_and_conditions);
    join->left_hash_.set_keys(left_keys);
    join->right_partition_.set_groups(right_keys);
    join->filter_.set_condition(filter_condition);
    return true;
}
bool ConditionOptimized::Transform(PhysicalOpNode* in,
                                   PhysicalOpNode** output) {
    *output = in;
    switch (in->type_) {
        case kPhysicalOpJoin: {
            PhysicalJoinNode* join_op = dynamic_cast<PhysicalJoinNode*>(in);
            return JoinConditionOptimized(join_op, &join_op->join_);
        }
        case kPhysicalOpRequestJoin: {
            PhysicalRequestJoinNode* join_op =
                dynamic_cast<PhysicalRequestJoinNode*>(in);
            return JoinConditionOptimized(join_op, &join_op->join_);
        }
        default: {
            return false;
        }
    }
}

// Transform condition expression some sub conditions
// e.g.
// condition : sub_expr1 and sub_expr2 and sub expr3
// and_condition_list [sub_expr1, sub_expr2, sub_exor3]
bool ConditionOptimized::TransfromAndConditionList(
    const node::ExprNode* condition, node::ExprListNode* and_condition_list) {
    if (nullptr == condition) {
        LOG(WARNING) << "fail to transfron conditions: null condition";
        return false;
    }

    switch (condition->expr_type_) {
        case node::kExprUnary: {
            const node::UnaryExpr* expr =
                dynamic_cast<const node::UnaryExpr*>(condition);
            switch (expr->GetOp()) {
                case node::kFnOpBracket: {
                    return TransfromAndConditionList(expr->children_[0],
                                                     and_condition_list);
                }
                default: {
                    and_condition_list->AddChild(
                        const_cast<node::ExprNode*>(condition));
                    return true;
                }
            }
        }
        case node::kExprBinary: {
            const node::BinaryExpr* expr =
                dynamic_cast<const node::BinaryExpr*>(condition);
            switch (expr->GetOp()) {
                case node::kFnOpAnd: {
                    for (auto child : expr->children_) {
                        TransfromAndConditionList(child, and_condition_list);
                    }
                    return true;
                }
                // TODO(chenjing): NOT(expr OR expr) ==> AND (NOT expr) AND (NOT
                // expr)
                default: {
                    and_condition_list->AddChild(
                        const_cast<node::ExprNode*>(condition));
                    return true;
                }
            }
        }
        default: {
            and_condition_list->AddChild(
                const_cast<node::ExprNode*>(condition));
            return true;
        }
    }
}
// Transform equal condition to expression pair
// e.g. t1.col1 = t2.col1 -> pair(t1.col1, t2.col1)
bool ConditionOptimized::ExtractEqualExprPair(
    node::ExprNode* condition,
    std::pair<node::ExprNode*, node::ExprNode*>* expr_pair) {
    if (nullptr == expr_pair || nullptr == condition) {
        return false;
    }
    switch (condition->expr_type_) {
        case node::kExprUnary: {
            const node::UnaryExpr* expr =
                dynamic_cast<const node::UnaryExpr*>(condition);
            switch (expr->GetOp()) {
                case node::kFnOpBracket: {
                    return ExtractEqualExprPair(expr->children_[0], expr_pair);
                }
                default: {
                    return false;
                }
            }
        }
        case node::kExprBinary: {
            const node::BinaryExpr* expr =
                dynamic_cast<const node::BinaryExpr*>(condition);
            switch (expr->GetOp()) {
                case node::kFnOpEq: {
                    expr_pair->first = expr->children_[0];
                    expr_pair->second = expr->children_[1];
                    return true;
                }
                default: {
                    return false;
                }
            }
        }
        default: {
            return false;
        }
    }
}

// Return Equal Expression Pair
// Left Expr should belongs to first schema
bool ConditionOptimized::TransformEqualExprPair(
    const std::vector<std::pair<const std::string, const vm::Schema*>>
        name_schema_list,
    node::ExprListNode* and_conditions, node::ExprListNode* out_condition_list,
    std::vector<ExprPair>& condition_eq_pair) {  // NOLINT
    vm::SchemasContext ctx(name_schema_list);
    for (auto expr : and_conditions->children_) {
        std::pair<node::ExprNode*, node::ExprNode*> expr_pair;
        if (ExtractEqualExprPair(expr, &expr_pair)) {
            const RowSchemaInfo* info_left;
            const RowSchemaInfo* info_right;
            if (!ctx.ExprRefResolved(expr_pair.first, &info_left)) {
                out_condition_list->AddChild(expr);
                continue;
            }
            if (!ctx.ExprRefResolved(expr_pair.second, &info_right)) {
                out_condition_list->AddChild(expr);
                continue;
            }
            if (nullptr == info_left || nullptr == info_right) {
                out_condition_list->AddChild(expr);
                continue;
            }
            if (info_left == info_right) {
                out_condition_list->AddChild(expr);
                continue;
            }
            if (0 == info_left->idx_) {
                ExprPair pair = {expr_pair.first, info_left->idx_,
                                 expr_pair.second, info_right->idx_};
                condition_eq_pair.push_back(pair);
            } else if (0 == info_right->idx_) {
                ExprPair pair = {expr_pair.second, info_right->idx_,
                                 expr_pair.first, info_left->idx_};
                condition_eq_pair.push_back(pair);
            } else {
                out_condition_list->AddChild(expr);
                continue;
            }
        } else {
            out_condition_list->AddChild(expr);
        }
    }
    return !condition_eq_pair.empty();
}
void ConditionOptimized::SkipConstExpression(node::ExprListNode input,
                                             node::ExprListNode* output) {
    if (node::ExprListNullOrEmpty(&input)) {
        return;
    }
}
bool LimitOptimized::Transform(PhysicalOpNode* in, PhysicalOpNode** output) {
    *output = in;
    if (kPhysicalOpLimit != in->type_) {
        return false;
    }

    auto limit_op = dynamic_cast<PhysicalLimitNode*>(in);

    if (ApplyLimitCnt(in->producers()[0], limit_op->GetLimitCnt())) {
        limit_op->SetLimitOptimized(true);
        return true;
    } else {
        return false;
    }
}

bool LimitOptimized::ApplyLimitCnt(PhysicalOpNode* node, int32_t limit_cnt) {
    if (kPhysicalOpLimit == node->type_) {
        auto limit_op = dynamic_cast<PhysicalLimitNode*>(node);
        if (0 == node->GetLimitCnt() || limit_op->GetLimitCnt() > limit_cnt) {
            if (limit_op->GetLimitOptimized()) {
                return ApplyLimitCnt(node->producers()[0], limit_cnt);
            } else {
                limit_op->SetLimitCnt(limit_cnt);
            }
        }
        return true;
    }
    if (node->producers().empty()) {
        return false;
    }
    if (node->is_block_) {
        if (0 == node->GetLimitCnt() || node->GetLimitCnt() > limit_cnt) {
            node->SetLimitCnt(limit_cnt);
        }
        return true;
    } else {
        if (false == ApplyLimitCnt(node->producers()[0], limit_cnt)) {
            if (0 == node->GetLimitCnt() || node->GetLimitCnt() > limit_cnt) {
                node->SetLimitCnt(limit_cnt);
            }
        } else {
            return true;
        }
    }
    return false;
}
// This is primarily intended to be used on top-level WHERE (or JOIN/ON)
// clauses.  It can also be used on top-level CHECK constraints, for which
// pass is_check = true.  DO NOT call it on any expression that is not known
// to be one or the other, as it might apply inappropriate simplifications.
bool CanonicalizeExprTransformPass::Transform(node::ExprNode* in,
                                              node::ExprNode** output) {
    // 1. 忽略NULL以及OR中的False/AND中的TRUE
    // 2. 拉平谓词
    // 3. 清除重复ORs
    // 4.
    return false;
}
bool TransformUpPysicalPass::Apply(PhysicalOpNode* in, PhysicalOpNode** out) {
    if (nullptr == in || nullptr == out) {
        LOG(WARNING) << "fail to apply pass: input or output is null";
        return false;
    }
    auto producer = in->producers();
    for (size_t j = 0; j < producer.size(); ++j) {
        PhysicalOpNode* output = nullptr;
        if (Apply(producer[j], &output)) {
            in->UpdateProducer(j, output);
        }
    }
    return Transform(in, out);
}

bool GroupAndSortOptimized::TransformOrderExpr(
    const node::OrderByNode* order, const Schema& schema,
    const IndexSt& index_st, const node::OrderByNode** output) {
    if (nullptr == order || nullptr == output) {
        LOG(WARNING) << "fail to transform order expr : order expr or "
                        "data_provider op "
                        "or output is null";
        return false;
    }

    auto& ts_column = schema.Get(index_st.ts_pos);
    std::vector<std::string> columns;
    *output = order;

    bool succ = false;
    for (auto expr : order->order_by_->children_) {
        switch (expr->expr_type_) {
            case node::kExprColumnRef:
                columns.push_back(
                    dynamic_cast<node::ColumnRefNode*>(expr)->GetColumnName());
                if (ts_column.name() ==
                    dynamic_cast<node::ColumnRefNode*>(expr)->GetColumnName()) {
                    succ = true;
                }
                break;
            default: {
                break;
            }
        }
    }

    if (succ) {
        node::ExprListNode* expr_list = node_manager_->MakeExprList();
        for (auto expr : order->order_by_->children_) {
            switch (expr->expr_type_) {
                case node::kExprColumnRef: {
                    std::string column =
                        dynamic_cast<node::ColumnRefNode*>(expr)
                            ->GetColumnName();
                    // skip group when match index keys
                    if (ts_column.name() != column) {
                        expr_list->AddChild(expr);
                    }
                    break;
                }
                default: {
                    expr_list->AddChild(expr);
                }
            }
        }
        *output = dynamic_cast<node::OrderByNode*>(
            node_manager_->MakeOrderByNode(expr_list, order->is_asc_));
        return true;
    } else {
        return false;
    }
}

bool LeftJoinOptimized::Transform(PhysicalOpNode* in, PhysicalOpNode** output) {
    if (nullptr == in) {
        LOG(WARNING) << "LeftJoin optimized skip: node is null";
        return false;
    }
    if (in->producers().empty() ||
        kPhysicalOpJoin != in->producers()[0]->type_) {
        return false;
    }
    PhysicalJoinNode* join_op =
        dynamic_cast<PhysicalJoinNode*>(in->producers()[0]);

    if (node::kJoinTypeLeft != join_op->join_type_ &&
        node::kJoinTypeLast != join_op->join_type_) {
        // skip optimized for other join type
        return false;
    }
    switch (in->type_) {
        case kPhysicalOpGroupBy: {
            auto group_op = dynamic_cast<PhysicalGroupNode*>(in);
            if (node::ExprListNullOrEmpty(group_op->group_.groups_)) {
                LOG(WARNING)
                    << "LeftJoin optimized skip: groups is null or empty";
            }

            if (!CheckExprListFromSchema(
                    group_op->group_.groups_,
                    (join_op->GetProducers()[0]->output_schema_))) {
                return false;
            }
            auto group_expr = group_op->group_.groups_;
            // 符合优化条件
            PhysicalGroupNode* new_group_op =
                new PhysicalGroupNode(join_op->producers()[0], group_expr);
            PhysicalJoinNode* new_join_op =
                new PhysicalJoinNode(new_group_op, join_op->GetProducers()[1],
                                     join_op->join_type_, join_op->join_);
            node_manager_->RegisterNode(new_group_op);
            node_manager_->RegisterNode(new_join_op);
            *output = new_join_op;
            return true;
        }
        case kPhysicalOpSortBy: {
            auto sort_op = dynamic_cast<PhysicalSortNode*>(in);
            if (nullptr == sort_op->sort_.orders_ ||
                node::ExprListNullOrEmpty(sort_op->sort_.orders_->order_by_)) {
                LOG(WARNING)
                    << "LeftJoin optimized skip: order is null or empty";
            }
            if (!CheckExprListFromSchema(
                    sort_op->sort_.orders_->order_by_,
                    join_op->GetProducers()[0]->output_schema_)) {
                return false;
            }
            // 符合优化条件
            PhysicalSortNode* new_order_op =
                new PhysicalSortNode(join_op->producers()[0], sort_op->sort_);
            node_manager_->RegisterNode(new_order_op);
            PhysicalJoinNode* new_join_op =
                new PhysicalJoinNode(new_order_op, join_op->GetProducers()[1],
                                     join_op->join_type_, join_op->join_);
            node_manager_->RegisterNode(new_order_op);
            *output = new_join_op;
            return true;
        }
        case kPhysicalOpProject: {
            auto project_op = dynamic_cast<PhysicalProjectNode*>(in);
            if (kWindowAggregation != project_op->project_type_) {
                return false;
            }
            auto group_sort_op =
                dynamic_cast<PhysicalWindowAggrerationNode*>(in);
            if (node::ExprListNullOrEmpty(
                    group_sort_op->window_.group_.groups_) &&
                (nullptr == group_sort_op->window_.sort_.orders_ ||
                 node::ExprListNullOrEmpty(
                     group_sort_op->window_.sort_.orders_->order_by_))) {
                LOG(WARNING) << "LeftJoin group and sort optimized skip: both "
                                "order and groups are empty ";
            }
            if (!CheckExprListFromSchema(
                    group_sort_op->window_.group_.groups_,
                    (join_op->GetProducers()[0]->output_schema_))) {
                return false;
            }

            if (!CheckExprListFromSchema(
                    group_sort_op->window_.sort_.orders_->order_by_,
                    (join_op->GetProducers()[0]->output_schema_))) {
                return false;
            }
            // TODO(chenjing): window agg 和 left jion的优化需要支持window with
            // join
            *output = in;
            return false;
        }
        default: {
            return false;
        }
    }
}
bool LeftJoinOptimized::ColumnExist(const Schema& schema,
                                    const std::string& column_name) {
    for (int32_t i = 0; i < schema.size(); i++) {
        const type::ColumnDef& column = schema.Get(i);
        if (column_name == column.name()) {
            return true;
        }
    }
    return false;
}
bool LeftJoinOptimized::CheckExprListFromSchema(
    const node::ExprListNode* expr_list, const Schema& schema) {
    if (node::ExprListNullOrEmpty(expr_list)) {
        return true;
    }
    for (auto expr : expr_list->children_) {
        switch (expr->expr_type_) {
            case node::kExprColumnRef: {
                auto column = dynamic_cast<node::ColumnRefNode*>(expr);
                if (!ColumnExist(schema, column->GetColumnName())) {
                    return false;
                }
                break;
            }
            default: {
                // can't optimize when group by other expression
                return false;
            }
        }
    }
    return true;
}

RequestModeransformer::RequestModeransformer(
    node::NodeManager* node_manager, const std::string& db,
    const std::shared_ptr<Catalog>& catalog, ::llvm::Module* module)
    : BatchModeTransformer(node_manager, db, catalog, module) {}

RequestModeransformer::~RequestModeransformer() {}

// transform project plan in request mode
bool RequestModeransformer::TransformProjecPlantOp(
    const node::ProjectPlanNode* node, PhysicalOpNode** output,
    base::Status& status) {
    if (nullptr == node || nullptr == output) {
        status.msg = "input node or output node is null";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }
    PhysicalOpNode* depend = nullptr;
    if (!TransformPlanOp(node->GetChildren()[0], &depend, status)) {
        return false;
    }

    std::vector<PhysicalOpNode*> ops;
    for (auto iter = node->project_list_vec_.cbegin();
         iter != node->project_list_vec_.cend(); iter++) {
        fesql::node::ProjectListNode* project_list =
            dynamic_cast<fesql::node::ProjectListNode*>(*iter);

        PhysicalOpNode* project_op = nullptr;
        if (!TransformProjectOp(project_list, depend, &project_op, status)) {
            return false;
        }
        ops.push_back(project_op);
    }

    if (ops.empty()) {
        status.msg = "fail transform project op: empty projects";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }

    if (ops.size() == 1) {
        *output = ops[0];
        return true;
    } else {
        auto iter = ops.cbegin();

        PhysicalOpNode* join = new PhysicalRequestJoinNode(
            (*iter), *(++iter), ::fesql::node::kJoinTypeConcat);
        node_manager_->RegisterNode(join);
        iter++;
        for (; iter != ops.cend(); iter++) {
            join = new PhysicalRequestJoinNode(join, *iter,
                                               ::fesql::node::kJoinTypeConcat);
            node_manager_->RegisterNode(join);
        }

        auto project_list =
            node_manager_->MakeProjectListPlanNode(nullptr, false);
        uint32_t pos = 0;
        for (auto iter = node->pos_mapping_.cbegin();
             iter != node->pos_mapping_.cend(); iter++) {
            auto sub_project_list = dynamic_cast<node::ProjectListNode*>(
                node->project_list_vec_[iter->first]);

            auto project_node = dynamic_cast<node::ProjectNode*>(
                sub_project_list->GetProjects().at(iter->second));
            if (node::kExprAll == project_node->GetExpression()->expr_type_) {
                auto all_expr =
                    dynamic_cast<node::AllNode*>(project_node->GetExpression());
                if (all_expr->children_.empty()) {
                    // expand all expression if needed
                    for (auto column : depend->output_schema_) {
                        all_expr->children_.push_back(
                            node_manager_->MakeColumnRefNode(
                                column.name(), all_expr->GetRelationName()));
                    }
                }
                project_list->AddProject(
                    node_manager_->MakeRowProjectNode(pos, "*", all_expr));
            } else {
                project_list->AddProject(node_manager_->MakeRowProjectNode(
                    pos, project_node->GetName(),
                    node_manager_->MakeColumnRefNode(project_node->GetName(),
                                                     "")));
            }
            pos++;
        }

        return CreatePhysicalProjectNode(kRowProject, join, project_list,
                                         output, status);
    }
}

bool RequestModeransformer::TransformJoinOp(const node::JoinPlanNode* node,
                                            PhysicalOpNode** output,
                                            base::Status& status) {
    if (nullptr == node || nullptr == output) {
        status.msg = "input node or output node is null";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }
    PhysicalOpNode* left = nullptr;
    PhysicalOpNode* right = nullptr;
    if (!TransformPlanOp(node->GetChildren()[0], &left, status)) {
        return false;
    }
    if (!TransformPlanOp(node->GetChildren()[1], &right, status)) {
        return false;
    }
    *output = new PhysicalRequestJoinNode(left, right, node->join_type_,
                                          node->condition_);
    node_manager_->RegisterNode(*output);
    return true;
}
bool RequestModeransformer::TransformScanOp(const node::TablePlanNode* node,
                                            PhysicalOpNode** output,
                                            base::Status& status) {
    if (nullptr == node || nullptr == output) {
        status.msg = "input node or output node is null";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }
    if (node->IsPrimary()) {
        auto table = catalog_->GetTable(db_, node->table_);
        if (table) {
            *output = new PhysicalRequestProviderNode(table);
            node_manager_->RegisterNode(*output);
            request_schema_ = *table->GetSchema();
            request_name_ = table->GetName();
            return true;
        } else {
            status.msg = "fail to transform data_provider op: table " + db_ +
                         "." + node->table_ + " not exist!";
            status.code = common::kPlanError;
            LOG(WARNING) << status.msg;
            return false;
        }
    }
    return BatchModeTransformer::TransformScanOp(node, output, status);
}
bool RequestModeransformer::TransformProjectOp(
    node::ProjectListNode* project_list, PhysicalOpNode* depend,
    PhysicalOpNode** output, base::Status& status) {
    if (nullptr != project_list->w_ptr_) {
        if (!TransformWindowOp(depend, project_list->w_ptr_, &depend, status)) {
            return false;
        }
    }
    switch (depend->output_type_) {
        case kSchemaTypeRow:
            return CreatePhysicalProjectNode(kRowProject, depend, project_list,
                                             output, status);
        case kSchemaTypeGroup:
            return CreatePhysicalProjectNode(kGroupAggregation, depend,
                                             project_list, output, status);
        case kSchemaTypeTable:
            if (project_list->is_window_agg_) {
                return CreatePhysicalProjectNode(kAggregation, depend,
                                                 project_list, output, status);
            } else {
                return CreatePhysicalProjectNode(kTableProject, depend,
                                                 project_list, output, status);
            }
    }
    return false;
}

}  // namespace vm
}  // namespace fesql
