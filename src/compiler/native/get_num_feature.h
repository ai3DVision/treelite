inline std::string get_num_feature_func(int num_feature) {
  std::ostringstream oss;
  oss << "size_t get_num_feature(void) {\n"
      << "  return " << num_feature << ";\n"
      << "}\n";
  return oss.str();
}

inline std::string get_num_feature_func_prototype() {
  return "size_t get_num_feature(void);\n";
}