#include <treelite/compiler.h>
#include <treelite/common.h>
#include <fmt/format.h>
#include <unordered_map>
#include <cmath>
#include "./param.h"
#include "./pred_transform.h"
#include "./ast/builder.h"

using namespace fmt::literals;

namespace treelite {
namespace compiler {

DMLC_REGISTRY_FILE_TAG(ast_java);

class ASTJavaCompiler : public Compiler {
 public:
  explicit ASTJavaCompiler(const CompilerParam& param)
    : param(param), file_prefix_(param.java_file_prefix) {
    if (param.verbose > 0) {
      LOG(INFO) << "Using ASTJavaCompiler";
    }
    if (file_prefix_[file_prefix_.length() - 1] != '/') {
      // Add missing last forward slash
      file_prefix_ += "/";
    }
  }

  CompiledModel Compile(const Model& model) override {
    CompiledModel cm;
    cm.backend = "java";
    cm.files["Main.java"] = "";

    if (param.annotate_in != "NULL") {
      LOG(WARNING) << "\033[1;31mWARNING: Java backend does not support "
                   << "branch annotation.\u001B[0m The parameter "
                   << "\x1B[33mannotate_in\u001B[0m will be ignored.";
    }
    num_output_group_ = model.num_output_group;
    pred_tranform_func_ = PredTransformFunction("java", model);
    files_.clear();

    ASTBuilder builder;
    builder.Build(model);
    builder.Split(param.parallel_comp);
    if (param.quantize > 0) {
      builder.QuantizeThresholds();
    }
    builder.CountDescendant();
    builder.BreakUpLargeUnits(param.max_unit_size);
    #include "java/entry_type.h"
    #include "java/pom_xml.h"
    files_[file_prefix_ + "Entry.java"]
      = fmt::format(entry_type_template,
          "java_package"_a = param.java_package);
    files_["pom.xml"]
      = fmt::format(pom_xml_template,
          "java_package"_a = param.java_package,
          "java_package_version"_a = param.java_package_version);
    WalkAST(builder.GetRootNode(), "Main.java", 0);

    cm.files = std::move(files_);
    cm.file_prefix = file_prefix_;
    return cm;
  }
 private:
  CompilerParam param;
  int num_output_group_;
  std::string pred_tranform_func_;
  std::unordered_map<std::string, std::string> files_;
  std::string main_tail_;
  std::string file_prefix_;

  void WalkAST(const ASTNode* node,
               const std::string& dest,
               size_t indent) {
    const MainNode* t1;
    const AccumulatorContextNode* t2;
    const ConditionNode* t3;
    const OutputNode* t4;
    const TranslationUnitNode* t5;
    const QuantizerNode* t6;
    if ( (t1 = dynamic_cast<const MainNode*>(node)) ) {
      HandleMainNode(t1, dest, indent);
    } else if ( (t2 = dynamic_cast<const AccumulatorContextNode*>(node)) ) {
      HandleACNode(t2, dest, indent);
    } else if ( (t3 = dynamic_cast<const ConditionNode*>(node)) ) {
      HandleCondNode(t3, dest, indent);
    } else if ( (t4 = dynamic_cast<const OutputNode*>(node)) ) {
      HandleOutputNode(t4, dest, indent);
    } else if ( (t5 = dynamic_cast<const TranslationUnitNode*>(node)) ) {
      HandleTUNode(t5, dest, indent);
    } else if ( (t6 = dynamic_cast<const QuantizerNode*>(node)) ) {
      HandleQNode(t6, dest, indent);
    } else {
      LOG(FATAL) << "Unrecognized AST node type";
    }
  }

  // append content to a given buffer, with given level of indentation
  inline void AppendToBuffer(const std::string& dest,
                             const std::string& content,
                             size_t indent) {
    files_[file_prefix_ + dest]
      += common::IndentMultiLineString(content, indent);
  }

  void HandleMainNode(const MainNode* node,
                      const std::string& dest,
                      size_t indent) {
    const char* predict_function_signature
      = (num_output_group_ > 1) ?
          "public static long predict_multiclass(Entry[] data, "
                                                "boolean pred_margin, "
                                                "float[] result)"
        : "public static float predict(Entry[] data, boolean pred_margin)";
    #include "./java/main_template.h"
    AppendToBuffer(dest,
      fmt::format(main_start_template,
        "java_package"_a = param.java_package,
        "num_output_group"_a = num_output_group_,
        "num_feature"_a = node->num_feature,
        "pred_transform_function"_a = pred_tranform_func_,
        "predict_function_signature"_a = predict_function_signature), indent);
    CHECK_EQ(node->children.size(), 1);
    WalkAST(node->children[0], dest, indent + 4);

    const std::string optional_average_field
      = (node->average_result) ? fmt::format(" / {}", node->num_tree)
                               : std::string("");
    if (num_output_group_ > 1) {
      AppendToBuffer(dest,
        fmt::format(main_end_multiclass_template,
          "num_output_group"_a = num_output_group_,
          "optional_average_field"_a = optional_average_field,
          "global_bias"_a = common::ToStringHighPrecision(node->global_bias),
          "main_footer"_a = main_tail_),
        indent);
    } else {
      AppendToBuffer(dest,
        fmt::format(main_end_template,
          "optional_average_field"_a = optional_average_field,
          "global_bias"_a = common::ToStringHighPrecision(node->global_bias),
          "main_footer"_a = main_tail_),
        indent);
    }
  }

  void HandleACNode(const AccumulatorContextNode* node,
                    const std::string& dest,
                    size_t indent) {
    if (num_output_group_ > 1) {
      AppendToBuffer(dest,
        fmt::format("float[] sum = new float[{num_output_group}];\n"
                    "int tmp;\n",
          "num_output_group"_a = num_output_group_), indent);
    } else {
      AppendToBuffer(dest,
        fmt::format("float sum = 0.0f;\n"
                    "int tmp;\n",
          "num_output_group"_a = num_output_group_), indent);
    }
    for (ASTNode* child : node->children) {
      WalkAST(child, dest, indent);
    }
  }

  void HandleCondNode(const ConditionNode* node,
                      const std::string& dest,
                      size_t indent) {
    const NumericalConditionNode* t;
    std::string condition;
    if ( (t = dynamic_cast<const NumericalConditionNode*>(node)) ) {
      /* numerical split */
      condition = ExtractNumericalCondition(t);
    } else {   /* categorical split */
      const CategoricalConditionNode* t2
        = dynamic_cast<const CategoricalConditionNode*>(node);
      CHECK(t2);
      condition = ExtractCategoricalCondition(t2);
    }

    const char* condition_with_na_check_template
      = (node->default_left) ?
          "!(data[{split_index}].missing.get() != -1) || ({condition})"
        : " (data[{split_index}].missing.get() != -1) && ({condition})";
    const std::string condition_with_na_check
      = fmt::format(condition_with_na_check_template,
          "split_index"_a = node->split_index,
          "condition"_a = condition);
    AppendToBuffer(dest,
      fmt::format("if ({} ) {{\n", condition_with_na_check), indent);
    CHECK_EQ(node->children.size(), 2);
    WalkAST(node->children[0], dest, indent + 2);
    AppendToBuffer(dest, "} else {\n", indent);
    WalkAST(node->children[1], dest, indent + 2);
    AppendToBuffer(dest, "}\n", indent);
  }

  void HandleOutputNode(const OutputNode* node,
                        const std::string& dest,
                        size_t indent) {
    std::string output_statement;
    if (num_output_group_ > 1) {
      if (node->is_vector) {
        // multi-class classification with random forest
        CHECK_EQ(node->vector.size(), static_cast<size_t>(num_output_group_))
          << "Ill-formed model: leaf vector must be of length [num_output_group]";
        for (int group_id = 0; group_id < num_output_group_; ++group_id) {
          output_statement
            += fmt::format("sum[{group_id}] += {output}f;\n",
                 "group_id"_a = group_id,
                 "output"_a
                   = common::ToStringHighPrecision(node->vector[group_id]));
        }
      } else {
        // multi-class classification with gradient boosted trees
        output_statement
          = fmt::format("sum[{group_id}] += {output}f;\n",
              "group_id"_a = node->tree_id % num_output_group_,
              "output"_a = common::ToStringHighPrecision(node->scalar));
      }
    } else {
      output_statement
        = fmt::format("sum += {output}f;\n",
            "output"_a = common::ToStringHighPrecision(node->scalar));
    }
    AppendToBuffer(dest, output_statement, indent);
    CHECK_EQ(node->children.size(), 0);
  }

  void HandleTUNode(const TranslationUnitNode* node,
                    const std::string& dest,
                    size_t indent) {
    const int unit_id = node->unit_id;
    const std::string class_name = fmt::format("TU{}", unit_id);
    const std::string new_file = fmt::format("{}.java", class_name);
    std::string unit_function_name, unit_function_signature,
                unit_function_call_signature;

    if (num_output_group_ > 1) {
      unit_function_name
        = fmt::format("predict_margin_multiclass_unit{}", unit_id);
      unit_function_signature
        = fmt::format("public static void {}(Entry[] data, float[] result)",
            unit_function_name);
      unit_function_call_signature
        = fmt::format("{class_name}.{unit_function_name}(data, sum);\n",
            "class_name"_a = class_name,
            "unit_function_name"_a = unit_function_name);
    } else {
      unit_function_name
        = fmt::format("predict_margin_unit{}", unit_id);
      unit_function_signature
        = fmt::format("public static float {}(Entry[] data)",
            unit_function_name);
      unit_function_call_signature
        = fmt::format("sum += {class_name}.{unit_function_name}(data);\n",
            "class_name"_a = class_name,
            "unit_function_name"_a = unit_function_name);
    }
    AppendToBuffer(dest, unit_function_call_signature, indent);
    AppendToBuffer(new_file,
                   fmt::format("package {java_package};\n\n"
                               "public class {class_name} {{\n"
                               "  {unit_function_signature} {{\n",
                     "java_package"_a = param.java_package,
                     "class_name"_a = class_name,
                     "unit_function_signature"_a = unit_function_signature), 0);
    CHECK_EQ(node->children.size(), 1);
    WalkAST(node->children[0], new_file, 4);
    if (num_output_group_ > 1) {
      AppendToBuffer(new_file,
        fmt::format("    for (int i = 0; i < {num_output_group}; ++i) {{\n"
                    "      result[i] = sum[i];\n"
                    "    }}\n"
                    "  }}\n"
                    "}}\n",
          "num_output_group"_a = num_output_group_), 0);
    } else {
      AppendToBuffer(new_file, "    return sum;\n  }\n}\n", 0);
    }
  }

  void HandleQNode(const QuantizerNode* node,
                   const std::string& dest,
                   size_t indent) {
    const int num_feature = node->is_categorical.size();
    /* render arrays needed to convert feature values into bin indices */
    std::string array_is_categorical, array_threshold,
                array_th_begin, array_th_len;
    // is_categorical[i] : is i-th feature categorical?
    {
      common::ArrayFormatter formatter(78, 2);
      for (int fid = 0; fid < num_feature; ++fid) {
        formatter << (node->is_categorical[fid] ? "true" : "false");
      }
      array_is_categorical = formatter.str();
    }
    // threshold[] : list of all thresholds that occur at least once in the
    //   ensemble model. For each feature, an ascending list of unique
    //   thresholds is generated. The range th_begin[i]:(th_begin[i]+th_len[i])
    //   of the threshold[] array stores the threshold list for feature i.
    {
      common::ArrayFormatter formatter(78, 2);
      for (const auto& e : node->cut_pts) {
        // cut_pts had been generated in ASTBuilder::QuantizeThresholds
        // cut_pts[i][k] stores the k-th threshold of feature i.
        for (tl_float v : e) {
          formatter << (common::ToStringHighPrecision(v) + "f");
        }
      }
      array_threshold = formatter.str();
    }
    {
      common::ArrayFormatter formatter(78, 2);
      size_t accum = 0;  // used to compute cumulative sum over threshold counts
      for (const auto& e : node->cut_pts) {
        formatter << accum;
        accum += e.size();  // e.size() = number of thresholds for each feature
      }
      array_th_begin = formatter.str();
    }
    {
      common::ArrayFormatter formatter(78, 2);
      for (const auto& e : node->cut_pts) {
        formatter << e.size();
      }
      array_th_len = formatter.str();
    }

    #include "./java/qnode_template.h"
    main_tail_
      += common::IndentMultiLineString(
           fmt::format(qnode_template,
             "array_is_categorical"_a = array_is_categorical,
             "array_threshold"_a = array_threshold,
             "array_th_begin"_a = array_th_begin,
             "array_th_len"_a = array_th_len), 2);
    AppendToBuffer(dest,
      fmt::format(quantize_loop_template,
        "num_feature"_a = num_feature), indent);
    CHECK_EQ(node->children.size(), 1);
    WalkAST(node->children[0], dest, indent);
  }

  inline std::vector<uint64_t>
  to_bitmap(const std::vector<uint32_t>& left_categories) const {
    const size_t num_left_categories = left_categories.size();
    const uint32_t max_left_category = left_categories[num_left_categories - 1];
    std::vector<uint64_t> bitmap((max_left_category + 1 + 63) / 64, 0);
    for (size_t i = 0; i < left_categories.size(); ++i) {
      const uint32_t cat = left_categories[i];
      const size_t idx = cat / 64;
      const uint32_t offset = cat % 64;
      bitmap[idx] |= (static_cast<uint64_t>(1) << offset);
    }
    return bitmap;
  }

  inline std::string
  ExtractNumericalCondition(const NumericalConditionNode* node) {
    std::string result;
    if (node->quantized) {  // quantized threshold
      result = fmt::format(
                   "data[{split_index}].qvalue.get() {opname} {threshold}",
                 "split_index"_a = node->split_index,
                 "opname"_a = OpName(node->op),
                 "threshold"_a = node->threshold.int_val);
    } else if (std::isinf(node->threshold.float_val)) {  // infinite threshold
      // According to IEEE 754, the result of comparison [lhs] < infinity
      // must be identical for all finite [lhs]. Same goes for operator >.
      result = (common::CompareWithOp(0.0, node->op, node->threshold.float_val)
                ? "true" : "false");
    } else {  // finite threshold
      result = fmt::format(
                   "data[{split_index}].fvalue.get() {opname} {threshold}f",
                 "split_index"_a = node->split_index,
                 "opname"_a = OpName(node->op),
                 "threshold"_a
                   = common::ToStringHighPrecision(node->threshold.float_val));
    }
    return result;
  }

  inline std::string
  ExtractCategoricalCondition(const CategoricalConditionNode* node) {
    std::string result;
    std::vector<uint64_t> bitmap = to_bitmap(node->left_categories);
    CHECK_GE(bitmap.size(), 1);
    bool all_zeros = true;
    for (uint64_t e : bitmap) {
      all_zeros &= (e == 0);
    }
    if (all_zeros) {
      result = "0";
    } else {
      std::ostringstream oss;
      oss << "(tmp = (int)(data[" << node->split_index << "].fvalue) ), "
          << "(tmp >= 0 && tmp < 64 && (( (long)"
          << bitmap[0] << "L >> tmp) & 1) )";
      for (size_t i = 1; i < bitmap.size(); ++i) {
        oss << " || (tmp >= " << (i * 64)
            << " && tmp < " << ((i + 1) * 64)
            << " && (( (long)" << bitmap[i]
            << "L >>> (tmp - " << (i * 64) << ") ) & 1) )";
      }
      result = oss.str();
      return result;
    }
  }
};

TREELITE_REGISTER_COMPILER(ASTJavaCompiler, "ast_java")
.describe("AST-based compiler that produces Java code")
.set_body([](const CompilerParam& param) -> Compiler* {
    return new ASTJavaCompiler(param);
  });
}  // namespace compiler
}  // namespace treelite
