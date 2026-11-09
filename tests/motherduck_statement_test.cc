// Copyright (c) 2026 ADBC Drivers Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.hpp>

#include "motherduck_statement.h"
#include "type_resolver_init.h"

namespace adbcpq {

using ::testing::ElementsAre;

class ParameterCastTypesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(InternalAdbcInitializeTypeResolver(resolver_).ok());
  }

  // Add a parameter to the bind schema. The pointer is valid until the next call.
  ArrowSchema* AddParam(ArrowType type) {
    ArrowSchema* param = params_.emplace_back().get();
    ArrowSchemaInit(param);
    if (type != NANOARROW_TYPE_UNINITIALIZED) {
      EXPECT_EQ(ArrowSchemaSetType(param, type), NANOARROW_OK);
    }
    EXPECT_EQ(ArrowSchemaSetName(param, ("p" + std::to_string(params_.size())).c_str()),
              NANOARROW_OK);
    return param;
  }

  std::vector<std::string> CastTypes(const std::vector<Oid>& null_param_oids = {}) {
    nanoarrow::UniqueSchema bind;
    ArrowSchemaInit(bind.get());
    EXPECT_EQ(ArrowSchemaSetTypeStruct(bind.get(), static_cast<int64_t>(params_.size())),
              NANOARROW_OK);
    for (size_t i = 0; i < params_.size(); i++) {
      EXPECT_EQ(ArrowSchemaDeepCopy(params_[i].get(), bind->children[i]), NANOARROW_OK);
    }
    std::vector<std::string> types;
    EXPECT_TRUE(ParameterCastTypes(resolver_, *bind.get(), null_param_oids, &types).ok());
    return types;
  }

  Oid OidOf(PostgresTypeId type_id) { return resolver_.GetOID(type_id); }

  PostgresTypeResolver resolver_;
  std::vector<nanoarrow::UniqueSchema> params_;
};

TEST_F(ParameterCastTypesTest, Primitives) {
  AddParam(NANOARROW_TYPE_INT32);
  AddParam(NANOARROW_TYPE_DOUBLE);
  AddParam(NANOARROW_TYPE_LARGE_STRING);
  AddParam(NANOARROW_TYPE_BINARY);
  AddParam(NANOARROW_TYPE_BOOL);
  AddParam(NANOARROW_TYPE_DATE32);
  AddParam(NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO);
  ASSERT_EQ(ArrowSchemaSetTypeDateTime(AddParam(NANOARROW_TYPE_UNINITIALIZED),
                                       NANOARROW_TYPE_TIMESTAMP,
                                       NANOARROW_TIME_UNIT_MICRO, "UTC"),
            NANOARROW_OK);
  EXPECT_THAT(CastTypes(), ElementsAre("int4", "float8", "text", "bytea", "bool", "date",
                                       "interval", "timestamptz"));
}

TEST_F(ParameterCastTypesTest, Decimals) {
  ArrowSchema* d = AddParam(NANOARROW_TYPE_UNINITIALIZED);
  ASSERT_EQ(ArrowSchemaSetTypeDecimal(d, NANOARROW_TYPE_DECIMAL128, 10, 5), NANOARROW_OK);
  // Too wide for DuckDB
  d = AddParam(NANOARROW_TYPE_UNINITIALIZED);
  ASSERT_EQ(ArrowSchemaSetTypeDecimal(d, NANOARROW_TYPE_DECIMAL256, 50, 10),
            NANOARROW_OK);
  // A list keeps its elements' precision and scale.
  ArrowSchema* list = AddParam(NANOARROW_TYPE_LIST);
  ASSERT_EQ(
      ArrowSchemaSetTypeDecimal(list->children[0], NANOARROW_TYPE_DECIMAL128, 12, 2),
      NANOARROW_OK);
  EXPECT_THAT(CastTypes(), ElementsAre("NUMERIC(10, 5)", "", "NUMERIC(12, 2)[]"));
}

TEST_F(ParameterCastTypesTest, Nested) {
  ArrowSchema* list = AddParam(NANOARROW_TYPE_LIST);
  ASSERT_EQ(ArrowSchemaSetType(list->children[0], NANOARROW_TYPE_STRING), NANOARROW_OK);
  ArrowSchema* dict = AddParam(NANOARROW_TYPE_INT32);
  ASSERT_EQ(ArrowSchemaAllocateDictionary(dict), NANOARROW_OK);
  ArrowSchemaInit(dict->dictionary);
  ASSERT_EQ(ArrowSchemaSetType(dict->dictionary, NANOARROW_TYPE_STRING), NANOARROW_OK);
  // Not something a parameter can be
  ArrowSchema* map = AddParam(NANOARROW_TYPE_UNINITIALIZED);
  ASSERT_EQ(ArrowSchemaSetTypeStruct(map, 1), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(map->children[0], NANOARROW_TYPE_INT32), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(map->children[0], "a"), NANOARROW_OK);
  EXPECT_THAT(CastTypes(), ElementsAre("text[]", "text", ""));
}

TEST_F(ParameterCastTypesTest, AllNull) {
  AddParam(NANOARROW_TYPE_NA);
  AddParam(NANOARROW_TYPE_NA);
  AddParam(NANOARROW_TYPE_NA);
  AddParam(NANOARROW_TYPE_NA);
  // Without bound types, they're left uncast.
  EXPECT_THAT(CastTypes(), ElementsAre("", "", "", ""));

  PostgresType int4_array;
  ASSERT_EQ(resolver_.FindArray(OidOf(PostgresTypeId::kInt4), &int4_array, nullptr),
            NANOARROW_OK);
  EXPECT_THAT(CastTypes({OidOf(PostgresTypeId::kText), OidOf(PostgresTypeId::kNumeric),
                         int4_array.oid(), /*unknown=*/987654321}),
              ElementsAre("text", "numeric", "int4[]", ""));
}

}  // namespace adbcpq
