/*-------------------------------------------------------------------------
 * Copyright (C) 2019, 4paradigm
 * plan_node.h
 *
 * Author: chenjing
 * Date: 2019/10/24
 *--------------------------------------------------------------------------
 **/

#ifndef SRC_NODE_PLAN_NODE_H_
#define SRC_NODE_PLAN_NODE_H_

#include <glog/logging.h>
#include <node/sql_node.h>
#include <list>
#include <string>
#include <vector>
#include "node/node_enum.h"
namespace fesql {
namespace node {
std::string NameOfPlanNodeType(const PlanType &type);

class PlanNode {
 public:
    explicit PlanNode(PlanType type) : type_(type) {}

    virtual ~PlanNode() {}

    virtual bool AddChild(PlanNode *node) = 0;

    PlanType GetType() const { return type_; }

    std::vector<PlanNode *> &GetChildren() { return children_; }

    int GetChildrenSize() { return children_.size(); }

    friend std::ostream &operator<<(std::ostream &output, const PlanNode &thiz);

    virtual void Print(std::ostream &output, const std::string &tab) const;

 protected:
    PlanType type_;
    std::vector<PlanNode *> children_;
};

typedef std::vector<PlanNode *> PlanNodeList;

class LeafPlanNode : public PlanNode {
 public:
    explicit LeafPlanNode(PlanType type) : PlanNode(type) {}
    ~LeafPlanNode() {}
    virtual bool AddChild(PlanNode *node);
};

class UnaryPlanNode : public PlanNode {
 public:
    explicit UnaryPlanNode(PlanType type) : PlanNode(type) {}
    ~UnaryPlanNode() {}
    virtual bool AddChild(PlanNode *node);
    virtual void Print(std::ostream &output, const std::string &org_tab) const;
};

class BinaryPlanNode : public PlanNode {
 public:
    explicit BinaryPlanNode(PlanType type) : PlanNode(type) {}
    ~BinaryPlanNode() {}
    virtual bool AddChild(PlanNode *node);
    virtual void Print(std::ostream &output, const std::string &org_tab) const;
};

class MultiChildPlanNode : public PlanNode {
 public:
    explicit MultiChildPlanNode(PlanType type) : PlanNode(type) {}
    ~MultiChildPlanNode() {}
    virtual bool AddChild(PlanNode *node);
    virtual void Print(std::ostream &output, const std::string &org_tab) const;
};

class SelectPlanNode : public MultiChildPlanNode {
 public:
    SelectPlanNode() : MultiChildPlanNode(kPlanTypeSelect), limit_cnt_(-1) {}
    ~SelectPlanNode() {}
    int GetLimitCount() { return limit_cnt_; }

    void SetLimitCount(int count) { limit_cnt_ = count; }

 private:
    int limit_cnt_;
};

class ScanPlanNode : public UnaryPlanNode {
 public:
    ScanPlanNode(const std::string &table_name, PlanType scan_type)
        : UnaryPlanNode(kPlanTypeScan),
          scan_type_(scan_type),
          table_name(table_name),
          limit_cnt(-1) {}
    ~ScanPlanNode() {}

    PlanType GetScanType() { return scan_type_; }

    int GetLimit() { return limit_cnt; }

    void SetLimit(int limit) { limit_cnt = limit; }

    SQLNode *GetCondition() const { return condition; }

 private:
    // TODO(chenjing): OP tid
    PlanType scan_type_;
    std::string table_name;
    SQLNode *condition;
    int limit_cnt;
};

class LimitPlanNode : public MultiChildPlanNode {
 public:
    LimitPlanNode() : MultiChildPlanNode(kPlanTypeLimit) {}
    explicit LimitPlanNode(int limit_cnt)
        : MultiChildPlanNode(kPlanTypeLimit), limit_cnt_(limit_cnt) {}

    ~LimitPlanNode() {}

    int GetLimitCnt() { return limit_cnt_; }

    void SetLimitCnt(int limit_cnt) { limit_cnt_ = limit_cnt; }

 private:
    int limit_cnt_;
};

/**
 * TODO:
 * where 过滤: 暂时不用考虑
 * having: 对结果过滤
 *
 */
class FilterPlanNode : public UnaryPlanNode {
 public:
    FilterPlanNode() : UnaryPlanNode(kPlanTypeFilter), condition_(nullptr) {}
    ~FilterPlanNode();

 private:
    SQLNode *condition_;
};

class ProjectPlanNode : public LeafPlanNode {
 public:
    ProjectPlanNode()
        : LeafPlanNode(kProject),
          expression_(nullptr),
          name_(""),
          table_(""),
          w_("") {}

    ~ProjectPlanNode() {}
    void Print(std::ostream &output, const std::string &orgTab) const;

    void SetW(const std::string w) { w_ = w; }
    std::string GetW() const { return w_; }

    std::string GetTable() const { return table_; }

    void SetTable(const std::string table) { table_ = table; }

    std::string GetName() const { return name_; }

    void SetName(const std::string name) { name_ = name; }

    node::SQLNode *GetExpression() const { return expression_; }

    void SetExpression(node::SQLNode *expression) { expression_ = expression; }

    bool IsWindowProject() { return !w_.empty(); }

 private:
    node::SQLNode *expression_;
    std::string name_;
    std::string table_;
    std::string w_;
};

class ProjectListPlanNode : public MultiChildPlanNode {
 public:
    ProjectListPlanNode() : MultiChildPlanNode(kProjectList) {}
    ProjectListPlanNode(const std::string &table, const std::string &w)
        : MultiChildPlanNode(kProjectList), table_(table), w_(w) {}
    ~ProjectListPlanNode() {}
    void Print(std::ostream &output, const std::string &org_tab) const;

    PlanNodeList &GetProjects() { return projects; }
    void AddProject(ProjectPlanNode *project) { projects.push_back(project); }

    std::string GetTable() const { return table_; }

    std::string GetW() const { return w_; }

 private:
    PlanNodeList projects;
    std::string table_;
    std::string w_;
};

class CreatePlanNode : public LeafPlanNode {
 public:
    CreatePlanNode()
        : LeafPlanNode(kPlanTypeCreate), database_(""), table_name_("") {}
    ~CreatePlanNode() {}

    std::string GetDatabase() const { return database_; }

    void setDatabase(const std::string &database) { database_ = database; }

    std::string GetTableName() const { return table_name_; }

    void setTableName(const std::string &table_name) {
        table_name_ = table_name;
    }

    NodePointVector &GetColumnDescList() { return column_desc_list_; }
    void SetColumnDescList(const NodePointVector &column_desc_list) {
        column_desc_list_ = column_desc_list;
    }

 private:
    std::string database_;
    std::string table_name_;
    NodePointVector column_desc_list_;
};

void PrintPlanVector(std::ostream &output, const std::string &tab,
                     PlanNodeList vec, const std::string vector_name,
                     bool last_item);

void PrintPlanNode(std::ostream &output, const std::string &org_tab,
                   PlanNode *node_ptr, const std::string &item_name,
                   bool last_child);

}  // namespace node
}  // namespace fesql

#endif  // SRC_NODE_PLAN_NODE_H_
