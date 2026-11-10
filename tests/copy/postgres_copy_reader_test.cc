// Copyright (c) 2026 ADBC Drivers Contributors
//
// This file has been modified from its original version, which is
// under the Apache License:
//
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

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.hpp>

#include "copy/reader.h"
#include "postgres_copy_test_common.h"

namespace adbcpq {

class PostgresCopyStreamTester {
 public:
  ArrowErrorCode Init(const PostgresType& root_type, ArrowError* error = nullptr) {
    NANOARROW_RETURN_NOT_OK(reader_.Init(root_type));
    NANOARROW_RETURN_NOT_OK(reader_.InferOutputSchema("PostgreSQL Tester", error));
    NANOARROW_RETURN_NOT_OK(reader_.InitFieldReaders(error));
    return NANOARROW_OK;
  }

  ArrowErrorCode ReadAll(ArrowBufferView* data, ArrowError* error = nullptr) {
    NANOARROW_RETURN_NOT_OK(reader_.ReadHeader(data, error));

    int result;
    do {
      result = reader_.ReadRecord(data, error);
    } while (result == NANOARROW_OK);

    return result;
  }

  void GetSchema(ArrowSchema* out) { reader_.GetSchema(out); }

  ArrowErrorCode GetArray(ArrowArray* out, ArrowError* error = nullptr) {
    return reader_.GetArray(out, error);
  }

 private:
  PostgresCopyStreamReader reader_;
};

TEST(PostgresCopyUtilsTest, PostgresCopyReadBoolean) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyBoolean;
  data.size_bytes = sizeof(kTestPgCopyBoolean);

  auto col_type = PostgresType(PostgresTypeId::kBool);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);

  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyBoolean, sizeof(kTestPgCopyBoolean));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 3);
  ASSERT_EQ(array->n_children, 1);

  const uint8_t* validity =
      reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  const uint8_t* data_buffer =
      reinterpret_cast<const uint8_t*>(array->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_FALSE(ArrowBitGet(validity, 2));

  ASSERT_TRUE(ArrowBitGet(data_buffer, 0));
  ASSERT_FALSE(ArrowBitGet(data_buffer, 1));
  ASSERT_FALSE(ArrowBitGet(data_buffer, 2));
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadSmallInt) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopySmallInt;
  data.size_bytes = sizeof(kTestPgCopySmallInt);

  auto col_type = PostgresType(PostgresTypeId::kInt2);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopySmallInt, sizeof(kTestPgCopySmallInt));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 5);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto data_buffer = reinterpret_cast<const int16_t*>(array->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_TRUE(ArrowBitGet(validity, 2));
  ASSERT_TRUE(ArrowBitGet(validity, 3));
  ASSERT_FALSE(ArrowBitGet(validity, 4));

  ASSERT_EQ(data_buffer[0], -123);
  ASSERT_EQ(data_buffer[1], -1);
  ASSERT_EQ(data_buffer[2], 1);
  ASSERT_EQ(data_buffer[3], 123);
  ASSERT_EQ(data_buffer[4], 0);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadInteger) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyInteger;
  data.size_bytes = sizeof(kTestPgCopyInteger);

  auto col_type = PostgresType(PostgresTypeId::kInt4);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyInteger, sizeof(kTestPgCopyInteger));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 5);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto data_buffer = reinterpret_cast<const int32_t*>(array->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_TRUE(ArrowBitGet(validity, 2));
  ASSERT_TRUE(ArrowBitGet(validity, 3));
  ASSERT_FALSE(ArrowBitGet(validity, 4));

  ASSERT_EQ(data_buffer[0], -123);
  ASSERT_EQ(data_buffer[1], -1);
  ASSERT_EQ(data_buffer[2], 1);
  ASSERT_EQ(data_buffer[3], 123);
  ASSERT_EQ(data_buffer[4], 0);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadBigInt) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyBigInt;
  data.size_bytes = sizeof(kTestPgCopyBigInt);

  auto col_type = PostgresType(PostgresTypeId::kInt8);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyBigInt, sizeof(kTestPgCopyBigInt));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 5);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto data_buffer = reinterpret_cast<const int64_t*>(array->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_TRUE(ArrowBitGet(validity, 2));
  ASSERT_TRUE(ArrowBitGet(validity, 3));
  ASSERT_FALSE(ArrowBitGet(validity, 4));

  ASSERT_EQ(data_buffer[0], -123);
  ASSERT_EQ(data_buffer[1], -1);
  ASSERT_EQ(data_buffer[2], 1);
  ASSERT_EQ(data_buffer[3], 123);
  ASSERT_EQ(data_buffer[4], 0);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadReal) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyReal;
  data.size_bytes = sizeof(kTestPgCopyReal);

  auto col_type = PostgresType(PostgresTypeId::kFloat4);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyReal, sizeof(kTestPgCopyReal));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 5);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto data_buffer = reinterpret_cast<const float*>(array->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_TRUE(ArrowBitGet(validity, 2));
  ASSERT_TRUE(ArrowBitGet(validity, 3));
  ASSERT_FALSE(ArrowBitGet(validity, 4));

  ASSERT_FLOAT_EQ(data_buffer[0], -123.456f);
  ASSERT_EQ(data_buffer[1], -1);
  ASSERT_EQ(data_buffer[2], 1);
  ASSERT_FLOAT_EQ(data_buffer[3], 123.456f);
  ASSERT_EQ(data_buffer[4], 0);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadDoublePrecision) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyDoublePrecision;
  data.size_bytes = sizeof(kTestPgCopyDoublePrecision);

  auto col_type = PostgresType(PostgresTypeId::kFloat8);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyDoublePrecision,
            sizeof(kTestPgCopyDoublePrecision));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 5);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto data_buffer = reinterpret_cast<const double*>(array->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_TRUE(ArrowBitGet(validity, 2));
  ASSERT_TRUE(ArrowBitGet(validity, 3));
  ASSERT_FALSE(ArrowBitGet(validity, 4));

  ASSERT_DOUBLE_EQ(data_buffer[0], -123.456);
  ASSERT_EQ(data_buffer[1], -1);
  ASSERT_EQ(data_buffer[2], 1);
  ASSERT_DOUBLE_EQ(data_buffer[3], 123.456);
  ASSERT_EQ(data_buffer[4], 0);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadDate) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyDate;
  data.size_bytes = sizeof(kTestPgCopyDate);

  auto col_type = PostgresType(PostgresTypeId::kDate);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyDate, sizeof(kTestPgCopyDate));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 3);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto data_buffer = reinterpret_cast<const int32_t*>(array->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_FALSE(ArrowBitGet(validity, 2));

  ASSERT_EQ(data_buffer[0], -25567);
  ASSERT_EQ(data_buffer[1], 47482);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadTime) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyTime;
  data.size_bytes = sizeof(kTestPgCopyTime);

  auto col_type = PostgresType(PostgresTypeId::kTime);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyTime, sizeof(kTestPgCopyTime));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 4);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto data_buffer = reinterpret_cast<const int64_t*>(array->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_TRUE(ArrowBitGet(validity, 2));
  ASSERT_FALSE(ArrowBitGet(validity, 3));

  ASSERT_EQ(data_buffer[0], 0);
  ASSERT_EQ(data_buffer[1], 86399000000);
  ASSERT_EQ(data_buffer[2], 49376123456);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadNumeric) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyNumeric;
  data.size_bytes = sizeof(kTestPgCopyNumeric);

  auto col_type = PostgresType(PostgresTypeId::kNumeric);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyNumeric, sizeof(kTestPgCopyNumeric));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 9);
  ASSERT_EQ(array->n_children, 1);

  nanoarrow::UniqueSchema schema;
  tester.GetSchema(schema.get());

  nanoarrow::UniqueArrayView array_view;
  ASSERT_EQ(ArrowArrayViewInitFromSchema(array_view.get(), schema.get(), nullptr),
            NANOARROW_OK);
  ASSERT_EQ(array_view->children[0]->storage_type, NANOARROW_TYPE_STRING);
  ASSERT_EQ(ArrowArrayViewSetArray(array_view.get(), array.get(), nullptr), NANOARROW_OK);

  auto validity = array_view->children[0]->buffer_views[0].data.as_uint8;
  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_TRUE(ArrowBitGet(validity, 2));
  ASSERT_TRUE(ArrowBitGet(validity, 3));
  ASSERT_TRUE(ArrowBitGet(validity, 4));
  ASSERT_TRUE(ArrowBitGet(validity, 5));
  ASSERT_TRUE(ArrowBitGet(validity, 6));
  ASSERT_TRUE(ArrowBitGet(validity, 7));
  ASSERT_FALSE(ArrowBitGet(validity, 8));

  struct ArrowStringView item;
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 0);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "1000000");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 1);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "0.00001234");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 2);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "1.0000");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 3);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "-123.456");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 4);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "123.456");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 5);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "nan");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 6);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "-inf");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 7);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "inf");
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadNumeric16_10) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyNumeric16_10;
  data.size_bytes = sizeof(kTestPgCopyNumeric16_10);

  auto col_type = PostgresType(PostgresTypeId::kNumeric);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyNumeric16_10,
            sizeof(kTestPgCopyNumeric16_10));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 7);
  ASSERT_EQ(array->n_children, 1);

  nanoarrow::UniqueSchema schema;
  tester.GetSchema(schema.get());

  nanoarrow::UniqueArrayView array_view;
  ASSERT_EQ(ArrowArrayViewInitFromSchema(array_view.get(), schema.get(), nullptr),
            NANOARROW_OK);
  ASSERT_EQ(array_view->children[0]->storage_type, NANOARROW_TYPE_STRING);
  ASSERT_EQ(ArrowArrayViewSetArray(array_view.get(), array.get(), nullptr), NANOARROW_OK);

  auto validity = array_view->children[0]->buffer_views[0].data.as_uint8;
  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_TRUE(ArrowBitGet(validity, 2));
  ASSERT_TRUE(ArrowBitGet(validity, 3));
  ASSERT_TRUE(ArrowBitGet(validity, 4));
  ASSERT_TRUE(ArrowBitGet(validity, 5));
  ASSERT_FALSE(ArrowBitGet(validity, 6));

  struct ArrowStringView item;
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 0);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "0.0000000000");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 1);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "1.0123400000");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 2);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "1.0123456789");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 3);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "-1.0123400000");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 4);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "-1.0123456789");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 5);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "nan");
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadTimestamp) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyTimestamp;
  data.size_bytes = sizeof(kTestPgCopyTimestamp);

  auto col_type = PostgresType(PostgresTypeId::kTimestamp);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyTimestamp, sizeof(kTestPgCopyTimestamp));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 3);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto data_buffer = reinterpret_cast<const int64_t*>(array->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_FALSE(ArrowBitGet(validity, 3));

  ASSERT_EQ(data_buffer[0], -2208943504000000);
  ASSERT_EQ(data_buffer[1], 4102490096000000);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadInterval) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyInterval;
  data.size_bytes = sizeof(kTestPgCopyInterval);

  auto col_type = PostgresType(PostgresTypeId::kInterval);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyInterval, sizeof(kTestPgCopyInterval));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 3);
  ASSERT_EQ(array->n_children, 1);

  nanoarrow::UniqueSchema schema;
  tester.GetSchema(schema.get());

  nanoarrow::UniqueArrayView array_view;
  ASSERT_EQ(ArrowArrayViewInitFromSchema(array_view.get(), schema.get(), nullptr),
            NANOARROW_OK);
  ASSERT_EQ(array_view->children[0]->storage_type,
            NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO);
  ASSERT_EQ(ArrowArrayViewSetArray(array_view.get(), array.get(), nullptr), NANOARROW_OK);

  auto validity = array_view->children[0]->buffer_views[0].data.as_uint8;
  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_FALSE(ArrowBitGet(validity, 2));

  struct ArrowInterval interval;
  ArrowIntervalInit(&interval, NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO);
  ArrowArrayViewGetIntervalUnsafe(array_view->children[0], 0, &interval);
  ASSERT_EQ(interval.months, -1);
  ASSERT_EQ(interval.days, -2);
  ASSERT_EQ(interval.ns, -4000000000);
  ArrowArrayViewGetIntervalUnsafe(array_view->children[0], 1, &interval);
  ASSERT_EQ(interval.months, 1);
  ASSERT_EQ(interval.days, 2);
  ASSERT_EQ(interval.ns, 4000000000);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadText) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyText;
  data.size_bytes = sizeof(kTestPgCopyText);

  auto col_type = PostgresType(PostgresTypeId::kText);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyText, sizeof(kTestPgCopyText));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 3);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto offsets = reinterpret_cast<const int32_t*>(array->children[0]->buffers[1]);
  auto data_buffer = reinterpret_cast<const char*>(array->children[0]->buffers[2]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_FALSE(ArrowBitGet(validity, 2));

  ASSERT_EQ(offsets[0], 0);
  ASSERT_EQ(offsets[1], 3);
  ASSERT_EQ(offsets[2], 7);
  ASSERT_EQ(offsets[3], 7);

  ASSERT_EQ(std::string(data_buffer + 0, 3), "abc");
  ASSERT_EQ(std::string(data_buffer + 3, 4), "1234");
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadEnum) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyEnum;
  data.size_bytes = sizeof(kTestPgCopyEnum);

  auto col_type = PostgresType(PostgresTypeId::kEnum);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyEnum, sizeof(kTestPgCopyEnum));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 3);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto offsets = reinterpret_cast<const int32_t*>(array->children[0]->buffers[1]);
  auto data_buffer = reinterpret_cast<const char*>(array->children[0]->buffers[2]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_FALSE(ArrowBitGet(validity, 2));

  ASSERT_EQ(offsets[0], 0);
  ASSERT_EQ(offsets[1], 2);
  ASSERT_EQ(offsets[2], 5);
  ASSERT_EQ(offsets[3], 5);

  ASSERT_EQ(std::string(data_buffer + 0, 2), "ok");
  ASSERT_EQ(std::string(data_buffer + 2, 3), "sad");
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadJson) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyJson;
  data.size_bytes = sizeof(kTestPgCopyJson);

  auto col_type = PostgresType(PostgresTypeId::kJson);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyJson, sizeof(kTestPgCopyJson));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 3);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto offsets = reinterpret_cast<const int32_t*>(array->children[0]->buffers[1]);
  auto data_buffer = reinterpret_cast<const char*>(array->children[0]->buffers[2]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_FALSE(ArrowBitGet(validity, 2));

  ASSERT_EQ(offsets[0], 0);
  ASSERT_EQ(offsets[1], 9);
  ASSERT_EQ(offsets[2], 18);
  ASSERT_EQ(offsets[3], 18);

  ASSERT_EQ(std::string(data_buffer, 9), "[1, 2, 3]");
  ASSERT_EQ(std::string(data_buffer + 9, 9), "[4, 5, 6]");
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadJsonb) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyJsonb;
  data.size_bytes = sizeof(kTestPgCopyJsonb);

  auto col_type = PostgresType(PostgresTypeId::kJsonb);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  struct ArrowError error;
  ArrowErrorInit(&error);
  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data, &error), ENODATA) << error.message;
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyJsonb, sizeof(kTestPgCopyJsonb));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;

  ASSERT_EQ(tester.GetArray(array.get(), &error), NANOARROW_OK) << error.message;
  ASSERT_EQ(array->length, 3);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto offsets = reinterpret_cast<const int32_t*>(array->children[0]->buffers[1]);
  auto data_buffer = reinterpret_cast<const char*>(array->children[0]->buffers[2]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_FALSE(ArrowBitGet(validity, 2));

  ASSERT_EQ(offsets[0], 0);
  ASSERT_EQ(offsets[1], 9);
  ASSERT_EQ(offsets[2], 18);
  ASSERT_EQ(offsets[3], 18);

  ASSERT_EQ(std::string(data_buffer, 9), "[1, 2, 3]");
  ASSERT_EQ(std::string(data_buffer + 9, 9), "[4, 5, 6]");
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadBinary) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyBinary;
  data.size_bytes = sizeof(kTestPgCopyBinary);

  auto col_type = PostgresType(PostgresTypeId::kBytea);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyBinary, sizeof(kTestPgCopyBinary));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 5);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto offsets = reinterpret_cast<const int32_t*>(array->children[0]->buffers[1]);
  auto data_buffer = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[2]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_TRUE(ArrowBitGet(validity, 2));
  ASSERT_TRUE(ArrowBitGet(validity, 3));
  ASSERT_FALSE(ArrowBitGet(validity, 4));

  ASSERT_EQ(offsets[0], 0);
  ASSERT_EQ(offsets[1], 0);
  ASSERT_EQ(offsets[2], 2);
  ASSERT_EQ(offsets[3], 6);
  ASSERT_EQ(offsets[4], 8);
  ASSERT_EQ(offsets[5], 8);

  ASSERT_EQ(data_buffer[0], 0x00);
  ASSERT_EQ(data_buffer[1], 0x01);
  ASSERT_EQ(data_buffer[2], 0x01);
  ASSERT_EQ(data_buffer[3], 0x02);
  ASSERT_EQ(data_buffer[4], 0x03);
  ASSERT_EQ(data_buffer[5], 0x04);
  ASSERT_EQ(data_buffer[6], 0xfe);
  ASSERT_EQ(data_buffer[7], 0xff);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadArray) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyIntegerArray;
  data.size_bytes = sizeof(kTestPgCopyIntegerArray);

  auto col_type = PostgresType(PostgresTypeId::kInt4).Array();
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyIntegerArray,
            sizeof(kTestPgCopyIntegerArray));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 3);
  ASSERT_EQ(array->n_children, 1);
  ASSERT_EQ(array->children[0]->n_children, 1);
  ASSERT_EQ(array->children[0]->children[0]->length, 5);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto offsets = reinterpret_cast<const int32_t*>(array->children[0]->buffers[1]);
  auto data_buffer =
      reinterpret_cast<const int32_t*>(array->children[0]->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_FALSE(ArrowBitGet(validity, 2));

  ASSERT_EQ(offsets[0], 0);
  ASSERT_EQ(offsets[1], 2);
  ASSERT_EQ(offsets[2], 5);
  ASSERT_EQ(offsets[3], 5);

  ASSERT_EQ(data_buffer[0], -123);
  ASSERT_EQ(data_buffer[1], -1);
  ASSERT_EQ(data_buffer[2], 0);
  ASSERT_EQ(data_buffer[3], 1);
  ASSERT_EQ(data_buffer[4], 123);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadInt2vector) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyInt2vector;
  data.size_bytes = sizeof(kTestPgCopyInt2vector);

  auto col_type = PostgresType(PostgresTypeId::kInt2vector);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("empty", col_type);
  input_type.AppendChild("len1", col_type);
  input_type.AppendChild("len2", col_type);
  input_type.AppendChild("len4", col_type);

  PostgresCopyStreamTester tester;
  ArrowError error;
  ArrowErrorInit(&error);
  ASSERT_EQ(tester.Init(input_type, &error), NANOARROW_OK) << error.message;
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyInt2vector, sizeof(kTestPgCopyInt2vector));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 1);
  ASSERT_EQ(array->n_children, 4);

  for (int col = 0; col < 4; col++) {
    ASSERT_EQ(array->children[col]->n_children, 1);
  }

  {
    auto* child = array->children[0];
    ASSERT_EQ(child->children[0]->length, 0);
    auto offsets = reinterpret_cast<const int32_t*>(child->buffers[1]);
    auto data_buffer = reinterpret_cast<const int16_t*>(child->children[0]->buffers[1]);
    ASSERT_NE(data_buffer, nullptr);

    EXPECT_EQ(offsets[0], 0);
    EXPECT_EQ(offsets[1], 0);
  }
  {
    auto* child = array->children[1];
    ASSERT_EQ(child->children[0]->length, 1);
    auto offsets = reinterpret_cast<const int32_t*>(child->buffers[1]);
    auto data_buffer = reinterpret_cast<const int16_t*>(child->children[0]->buffers[1]);
    ASSERT_NE(data_buffer, nullptr);

    EXPECT_EQ(offsets[0], 0);
    EXPECT_EQ(offsets[1], 1);

    EXPECT_EQ(data_buffer[0], -32768);
  }
  {
    auto* child = array->children[2];
    ASSERT_EQ(child->children[0]->length, 2);
    auto offsets = reinterpret_cast<const int32_t*>(child->buffers[1]);
    auto data_buffer = reinterpret_cast<const int16_t*>(child->children[0]->buffers[1]);
    ASSERT_NE(data_buffer, nullptr);

    EXPECT_EQ(offsets[0], 0);
    EXPECT_EQ(offsets[1], 2);

    EXPECT_EQ(data_buffer[0], -32768);
    EXPECT_EQ(data_buffer[1], 32767);
  }
  {
    auto* child = array->children[3];
    ASSERT_EQ(child->children[0]->length, 4);
    auto offsets = reinterpret_cast<const int32_t*>(child->buffers[1]);
    auto data_buffer = reinterpret_cast<const int16_t*>(child->children[0]->buffers[1]);
    ASSERT_NE(data_buffer, nullptr);

    EXPECT_EQ(offsets[0], 0);
    EXPECT_EQ(offsets[1], 4);

    EXPECT_EQ(data_buffer[0], -1);
    EXPECT_EQ(data_buffer[1], 0);
    EXPECT_EQ(data_buffer[2], 1);
    EXPECT_EQ(data_buffer[3], 42);
  }
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadCustomRecord) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyCustomRecord;
  data.size_bytes = sizeof(kTestPgCopyCustomRecord);

  auto col_type = PostgresType(PostgresTypeId::kRecord);
  col_type.AppendChild("nested1", PostgresType(PostgresTypeId::kInt4));
  col_type.AppendChild("nested2", PostgresType(PostgresTypeId::kFloat8));
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyCustomRecord,
            sizeof(kTestPgCopyCustomRecord));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 3);
  ASSERT_EQ(array->n_children, 1);
  ASSERT_EQ(array->children[0]->n_children, 2);
  ASSERT_EQ(array->children[0]->children[0]->length, 3);
  ASSERT_EQ(array->children[0]->children[1]->length, 3);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto data_buffer1 =
      reinterpret_cast<const int32_t*>(array->children[0]->children[0]->buffers[1]);
  auto data_buffer2 =
      reinterpret_cast<const double*>(array->children[0]->children[1]->buffers[1]);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_FALSE(ArrowBitGet(validity, 2));

  ASSERT_EQ(data_buffer1[0], 123);
  ASSERT_EQ(data_buffer1[1], 12);
  ASSERT_EQ(data_buffer1[2], 0);

  ASSERT_DOUBLE_EQ(data_buffer2[0], 456.789);
  ASSERT_DOUBLE_EQ(data_buffer2[1], 345.678);
  ASSERT_DOUBLE_EQ(data_buffer2[2], 0);
}

TEST(PostgresCopyUtilsTest, BinaryOverflow) {
  constexpr int32_t field_size_bytes = 1024 * 1024 * 4;
  std::vector<uint8_t> buffer(field_size_bytes);

  nanoarrow::UniqueSchema schema;
  ASSERT_EQ(NANOARROW_OK, ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_BINARY));

  nanoarrow::UniqueArray array;
  ASSERT_EQ(NANOARROW_OK, ArrowArrayInitFromType(array.get(), NANOARROW_TYPE_BINARY));
  ASSERT_EQ(NANOARROW_OK, ArrowArrayStartAppending(array.get()));

  PostgresCopyBinaryFieldReader reader;
  ASSERT_EQ(NANOARROW_OK, reader.InitSchema(schema.get()));
  ASSERT_EQ(NANOARROW_OK, reader.InitArray(array.get()));
  for (int i = 0; i < 511; i++) {
    ArrowBufferView data;
    data.size_bytes = static_cast<int64_t>(buffer.size());
    data.data.data = buffer.data();
    ASSERT_EQ(NANOARROW_OK, reader.Read(&data, field_size_bytes, array.get(), nullptr));
  }
  ArrowBufferView data;
  data.size_bytes = static_cast<int64_t>(buffer.size());
  data.data.data = buffer.data();
  ASSERT_EQ(EOVERFLOW, reader.Read(&data, field_size_bytes, array.get(), nullptr));
}

TEST(PostgresCopyUtilsTest, JsonbOverflow) {
  constexpr int32_t field_size_bytes = 1024 * 1024 * 4;
  std::vector<uint8_t> buffer(field_size_bytes, 1);

  nanoarrow::UniqueSchema schema;
  ASSERT_EQ(NANOARROW_OK, ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_BINARY));

  nanoarrow::UniqueArray array;
  ASSERT_EQ(NANOARROW_OK, ArrowArrayInitFromType(array.get(), NANOARROW_TYPE_BINARY));
  ASSERT_EQ(NANOARROW_OK, ArrowArrayStartAppending(array.get()));

  PostgresCopyJsonbFieldReader reader;
  ASSERT_EQ(NANOARROW_OK, reader.InitSchema(schema.get()));
  ASSERT_EQ(NANOARROW_OK, reader.InitArray(array.get()));
  for (int i = 0; i < 512; i++) {
    ArrowBufferView data;
    data.size_bytes = static_cast<int64_t>(buffer.size());
    data.data.data = buffer.data();
    ASSERT_EQ(NANOARROW_OK, reader.Read(&data, field_size_bytes, array.get(), nullptr));
  }
  ArrowBufferView data;
  data.size_bytes = static_cast<int64_t>(buffer.size());
  data.data.data = buffer.data();
  ASSERT_EQ(EOVERFLOW, reader.Read(&data, field_size_bytes, array.get(), nullptr));
}

// A row whose last fixed-width field claims more bytes than remain in the
// buffer must fail instead of reading past the end of it. Each truncation test
// points at a valid fixture but cuts size_bytes short partway through the first
// row, so the bytes past the cut are still owned by the test.

// Fixture offset of the first field's data: header (19 bytes), field count
// (2 bytes), field length (4 bytes)
constexpr int64_t kFirstFieldDataOffset = 25;

// Fixture offset of the low byte of the first field's length
constexpr size_t kFirstFieldLengthLowByte = kFirstFieldDataOffset - 1;

void ExpectRecordFails(const PostgresType& col_type, const uint8_t* buffer,
                       int64_t size_bytes, const std::string& expected_message) {
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamReader reader;
  ASSERT_EQ(reader.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(reader.InferOutputSchema("PostgreSQL Tester", nullptr), NANOARROW_OK);
  ASSERT_EQ(reader.InitFieldReaders(nullptr), NANOARROW_OK);

  ArrowBufferView data;
  data.data.as_uint8 = buffer;
  data.size_bytes = size_bytes;

  ArrowError error;
  ArrowErrorInit(&error);
  ASSERT_EQ(reader.ReadHeader(&data, &error), NANOARROW_OK) << error.message;
  ASSERT_EQ(reader.ReadRecord(&data, &error), EINVAL);
  ASSERT_NE(std::string(error.message).find(expected_message), std::string::npos)
      << error.message;
}

void ExpectTruncatedRecordFails(const PostgresType& col_type, const uint8_t* buffer,
                                int64_t size_bytes) {
  ExpectRecordFails(col_type, buffer, size_bytes, "Unexpected end of input");
}

// Copy a fixture and overwrite the declared length of its first field (which
// is always less than 256 in these fixtures)
template <size_t N>
std::vector<uint8_t> WithFirstFieldLength(const uint8_t (&fixture)[N], uint8_t length) {
  std::vector<uint8_t> out(fixture, fixture + N);
  EXPECT_EQ(out[kFirstFieldLengthLowByte - 3], 0);
  EXPECT_EQ(out[kFirstFieldLengthLowByte - 2], 0);
  EXPECT_EQ(out[kFirstFieldLengthLowByte - 1], 0);
  out[kFirstFieldLengthLowByte] = length;
  return out;
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadTruncatedBoolean) {
  ExpectTruncatedRecordFails(PostgresType(PostgresTypeId::kBool), kTestPgCopyBoolean,
                             kFirstFieldDataOffset);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadTruncatedSmallInt) {
  ExpectTruncatedRecordFails(PostgresType(PostgresTypeId::kInt2), kTestPgCopySmallInt,
                             kFirstFieldDataOffset + 1);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadTruncatedInteger) {
  ExpectTruncatedRecordFails(PostgresType(PostgresTypeId::kInt4), kTestPgCopyInteger,
                             kFirstFieldDataOffset + 2);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadTruncatedReal) {
  ExpectTruncatedRecordFails(PostgresType(PostgresTypeId::kFloat4), kTestPgCopyReal,
                             kFirstFieldDataOffset + 3);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadTruncatedBigInt) {
  ExpectTruncatedRecordFails(PostgresType(PostgresTypeId::kInt8), kTestPgCopyBigInt,
                             kFirstFieldDataOffset + 3);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadTruncatedDoublePrecision) {
  ExpectTruncatedRecordFails(PostgresType(PostgresTypeId::kFloat8),
                             kTestPgCopyDoublePrecision, kFirstFieldDataOffset + 7);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadTruncatedInterval) {
  ExpectTruncatedRecordFails(PostgresType(PostgresTypeId::kInterval), kTestPgCopyInterval,
                             kFirstFieldDataOffset + 10);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadTruncatedArrayElement) {
  // The first array is 36 bytes; cut the last 2 bytes of its last element
  ExpectTruncatedRecordFails(PostgresType(PostgresTypeId::kInt4).Array(),
                             kTestPgCopyIntegerArray, kFirstFieldDataOffset + 36 - 2);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadTruncatedNumeric) {
  // The first numeric is 10 bytes (8-byte header + 1 digit); cut it after the
  // header
  ExpectTruncatedRecordFails(PostgresType(PostgresTypeId::kNumeric), kTestPgCopyNumeric,
                             kFirstFieldDataOffset + 8);
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadNumericDeclaredLengthTooShort) {
  // The first numeric has one digit (10 bytes). Declaring 8 bytes would make
  // its digit come out of the next field's length.
  auto buffer = WithFirstFieldLength(kTestPgCopyNumeric, 8);
  ExpectRecordFails(PostgresType(PostgresTypeId::kNumeric), buffer.data(),
                    static_cast<int64_t>(buffer.size()),
                    "Expected numeric field with 1 digits to have 10 bytes but found "
                    "field with 8 bytes");
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadNumericDeclaredLengthTooLong) {
  auto buffer = WithFirstFieldLength(kTestPgCopyNumeric, 12);
  ExpectRecordFails(PostgresType(PostgresTypeId::kNumeric), buffer.data(),
                    static_cast<int64_t>(buffer.size()),
                    "Expected numeric field with 1 digits to have 10 bytes but found "
                    "field with 12 bytes");
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadNumericShorterThanHeader) {
  auto buffer = WithFirstFieldLength(kTestPgCopyNumeric, 4);
  ExpectRecordFails(PostgresType(PostgresTypeId::kNumeric), buffer.data(),
                    static_cast<int64_t>(buffer.size()),
                    "Expected numeric field with at least 8 bytes");
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadJsonbEmptyField) {
  // A JSONB field must include its version byte
  auto buffer = WithFirstFieldLength(kTestPgCopyJsonb, 0);
  ExpectRecordFails(PostgresType(PostgresTypeId::kJsonb), buffer.data(),
                    static_cast<int64_t>(buffer.size()),
                    "Expected JSONB field with at least 1 byte");
}

TEST(PostgresCopyUtilsTest, PostgresCopyReadArrayInvalidDimensions) {
  PostgresType input_type(PostgresTypeId::kInt4);
  input_type = input_type.Array();
  PostgresCopyArrayFieldReader reader;
  reader.Init(input_type);
  reader.InitChild(std::make_unique<PostgresCopyNetworkEndianFieldReader<int32_t>>());

  nanoarrow::UniqueSchema schema;
  ASSERT_EQ(ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_LIST), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32), NANOARROW_OK);
  ASSERT_EQ(reader.InitSchema(schema.get()), NANOARROW_OK);

  // n_dim, flags, element oid, then (dim_size, lower_bound) pairs
  auto be32 = [](std::vector<uint8_t>* v, uint32_t x) {
    for (int shift = 24; shift >= 0; shift -= 8) v->push_back((x >> shift) & 0xff);
  };

  // Three dimensions of INT32_MAX multiply past INT64_MAX; negative dim_size
  for (std::vector<int32_t> dims : {std::vector<int32_t>{INT32_MAX, INT32_MAX, INT32_MAX},
                                    std::vector<int32_t>{-1}}) {
    nanoarrow::UniqueArray array;
    ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), NANOARROW_OK);
    ASSERT_EQ(ArrowArrayStartAppending(array.get()), NANOARROW_OK);
    ASSERT_EQ(reader.InitArray(array.get()), NANOARROW_OK);

    std::vector<uint8_t> buf;
    be32(&buf, static_cast<uint32_t>(dims.size()));
    be32(&buf, 0);
    be32(&buf, 23);
    for (int32_t d : dims) {
      be32(&buf, static_cast<uint32_t>(d));
      be32(&buf, 1);
    }

    ArrowBufferView data;
    data.data.as_uint8 = buf.data();
    data.size_bytes = buf.size();
    ArrowError error;
    ASSERT_EQ(reader.Read(&data, static_cast<int32_t>(buf.size()), array.get(), &error),
              EINVAL);
    ASSERT_NE(std::string(error.message).find("Invalid array dimension"),
              std::string::npos)
        << error.message;
  }
}

// N.B. unfortunately testing with NUMERIC is difficult since we can't add all
// that much to the buffer on each iteration, so the unit test takes too long

}  // namespace adbcpq
