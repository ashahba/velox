/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "velox/functions/prestosql/aggregates/MapUnionSumAggregate.h"
#include "velox/exec/AddressableNonNullValueList.h"
#include "velox/exec/Aggregate.h"
#include "velox/exec/Strings.h"
#include "velox/expression/FunctionSignature.h"
#include "velox/functions/prestosql/aggregates/AggregateNames.h"
#include "velox/vector/FlatVector.h"

namespace facebook::velox::aggregate::prestosql {

namespace {

template <typename K, typename S>
struct Accumulator {
  using ValuesMap = typename util::floating_point::HashMapNaNAwareTypeTraits<
      K,
      S,
      AlignedStlAllocator<std::pair<const K, S>, 16>>::Type;
  ValuesMap sums;

  explicit Accumulator(const TypePtr& /*type*/, HashStringAllocator* allocator)
      : sums{AlignedStlAllocator<std::pair<const K, S>, 16>(allocator)} {}

  size_t size() const {
    return sums.size();
  }

  void addValues(
      const MapVector* mapVector,
      const VectorPtr& mapKeys,
      const VectorPtr& mapValues,
      vector_size_t row,
      HashStringAllocator* allocator) {
    auto keys = mapKeys->template as<SimpleVector<K>>();
    auto values = mapValues->template as<SimpleVector<S>>();
    auto offset = mapVector->offsetAt(row);
    auto size = mapVector->sizeAt(row);

    for (auto i = 0; i < size; ++i) {
      // Ignore null map keys.
      if (!keys->isNullAt(offset + i)) {
        auto key = keys->valueAt(offset + i);
        addValue(key, values, offset + i, values->typeKind());
      }
    }
  }

  void addValue(
      K key,
      const SimpleVector<S>* mapValues,
      vector_size_t row,
      TypeKind valueKind) {
    if (mapValues->isNullAt(row)) {
      sums[key] += 0;
    } else {
      auto value = mapValues->valueAt(row);

      if constexpr (std::is_same_v<S, double> || std::is_same_v<S, float>) {
        sums[key] += value;
      } else {
        S checkedSum;
        auto overflow = __builtin_add_overflow(sums[key], value, &checkedSum);

        if (UNLIKELY(overflow)) {
          auto errorValue = (int128_t(sums[key]) + int128_t(value));

          if (errorValue < 0) {
            VELOX_ARITHMETIC_ERROR(
                "Value {} is less than {}",
                errorValue,
                std::numeric_limits<S>::min());
          } else {
            VELOX_ARITHMETIC_ERROR(
                "Value {} exceeds {}",
                errorValue,
                std::numeric_limits<S>::max());
          }
        }
        sums[key] = checkedSum;
      }
    }
  }

  vector_size_t extractValues(
      VectorPtr& mapKeys,
      VectorPtr& mapValues,
      vector_size_t offset) {
    auto keys = mapKeys->asFlatVector<K>();
    auto values = mapValues->asFlatVector<S>();

    auto index = offset;
    for (const auto& [key, sum] : sums) {
      keys->set(index, key);
      values->set(index, sum);

      ++index;
    }

    return sums.size();
  }
};

template <typename S>
struct StringViewAccumulator {
  Accumulator<StringView, S> base;

  Strings strings;

  explicit StringViewAccumulator(
      const TypePtr& type,
      HashStringAllocator* allocator)
      : base{type, allocator} {}

  size_t size() const {
    return base.size();
  }

  void addValues(
      const MapVector* mapVector,
      const VectorPtr& mapKeys,
      const VectorPtr& mapValues,
      vector_size_t row,
      HashStringAllocator* allocator) {
    auto keys = mapKeys->template as<SimpleVector<StringView>>();
    auto values = mapValues->template as<SimpleVector<S>>();
    auto offset = mapVector->offsetAt(row);
    auto size = mapVector->sizeAt(row);

    for (auto i = 0; i < size; ++i) {
      // Ignore null map keys.
      if (!keys->isNullAt(offset + i)) {
        auto key = keys->valueAt(offset + i);

        if (!key.isInline()) {
          auto it = base.sums.find(key);
          if (it != base.sums.end()) {
            key = it->first;
          } else {
            key = strings.append(key, *allocator);
          }
        }

        base.addValue(key, values, offset + i, values->typeKind());
      }
    }
  }

  vector_size_t extractValues(
      VectorPtr& mapKeys,
      VectorPtr& mapValues,
      vector_size_t offset) {
    return base.extractValues(mapKeys, mapValues, offset);
  }
};

/// Maintains a map with keys of type array, map or struct.
template <typename V>
struct ComplexTypeAccumulator {
  using ValueMap = folly::F14FastMap<
      AddressableNonNullValueList::Entry,
      V,
      AddressableNonNullValueList::Hash,
      AddressableNonNullValueList::EqualTo,
      AlignedStlAllocator<
          std::pair<const AddressableNonNullValueList::Entry, V>,
          16>>;

  /// A set of pointers to values stored in AddressableNonNullValueList.
  ValueMap sums;

  /// Stores unique non-null keys.
  AddressableNonNullValueList serializedKeys;

  ComplexTypeAccumulator(const TypePtr& type, HashStringAllocator* allocator)
      : sums{
            0,
            AddressableNonNullValueList::Hash{},
            AddressableNonNullValueList::EqualTo{type->asMap().keyType()},
            AlignedStlAllocator<
                std::pair<const AddressableNonNullValueList::Entry, V>,
                16>(allocator)} {}

  void addValues(
      const MapVector* mapVector,
      const VectorPtr& mapKeys,
      const VectorPtr& mapValues,
      vector_size_t row,
      HashStringAllocator* allocator) {
    auto offset = mapVector->offsetAt(row);
    auto size = mapVector->sizeAt(row);
    auto values = mapValues->template as<SimpleVector<V>>();

    for (auto i = 0; i < size; ++i) {
      if (!mapKeys->isNullAt(offset + i) && (offset + i) < mapKeys->size()) {
        // Get entry and value to add.
        auto entry =
            serializedKeys.append(*mapKeys.get(), offset + i, allocator);
        auto value =
            (values->isNullAt(offset + i)) ? 0 : values->valueAt(offset + i);

        // New entry.
        if (!sums.contains(entry)) {
          sums[entry] = value;
        } else {
          // Existing entry.
          if constexpr (std::is_same_v<V, double> || std::is_same_v<V, float>) {
            sums[entry] += value;
          } else {
            V checkedSum;
            auto overflow =
                __builtin_add_overflow(sums[entry], value, &checkedSum);

            if (UNLIKELY(overflow)) {
              auto errorValue = (int128_t(sums[entry]) + int128_t(value));

              if (errorValue < 0) {
                VELOX_ARITHMETIC_ERROR(
                    "Value {} is less than {}",
                    errorValue,
                    std::numeric_limits<V>::min());
              } else {
                VELOX_ARITHMETIC_ERROR(
                    "Value {} exceeds {}",
                    errorValue,
                    std::numeric_limits<V>::max());
              }
            }
            sums[entry] = checkedSum;
            serializedKeys.removeLast(entry);
          }
        }
      }
    }
  }

  vector_size_t extractValues(
      VectorPtr& mapKeys,
      VectorPtr& mapValues,
      vector_size_t offset) {
    auto values = mapValues->asFlatVector<V>();
    auto index = offset;

    for (const auto& [position, count] : sums) {
      AddressableNonNullValueList::read(position, *mapKeys.get(), index);
      values->set(index, count);
      ++index;
    }

    return sums.size();
  }

  size_t size() const {
    return sums.size();
  }

  void free(HashStringAllocator& allocator) {
    serializedKeys.free(allocator);
    std::destroy_at(&sums);
  }
};

// Defines unique accumulators dependent on type.
template <typename K, typename S>
struct AccumulatorTypeTraits {
  using AccumulatorType = Accumulator<K, S>;
};

template <typename S>
struct AccumulatorTypeTraits<StringView, S> {
  using AccumulatorType = StringViewAccumulator<S>;
};

template <typename V>
struct AccumulatorTypeTraits<ComplexType, V> {
  using AccumulatorType = ComplexTypeAccumulator<V>;
};

template <typename S>
struct AccumulatorTypeTraits<UnknownValue, S> {
  using AccumulatorType = Accumulator<int32_t, S>;
};

// Defines common aggregator.
template <typename K, typename S>
class MapUnionSumAggregate : public exec::Aggregate {
 public:
  explicit MapUnionSumAggregate(TypePtr resultType)
      : Aggregate(std::move(resultType)) {}

  using AccumulatorType = typename AccumulatorTypeTraits<K, S>::AccumulatorType;

  int32_t accumulatorFixedWidthSize() const override {
    return sizeof(AccumulatorType);
  }

  bool isFixedSize() const override {
    return false;
  }

  void extractValues(char** groups, int32_t numGroups, VectorPtr* result)
      override {
    auto mapVector = (*result)->as<MapVector>();
    VELOX_CHECK(mapVector);
    mapVector->resize(numGroups);

    auto mapKeysPtr = mapVector->mapKeys();
    auto mapValuesPtr = mapVector->mapValues();

    auto numElements = countElements(groups, numGroups);
    mapVector->mapValues()->as<FlatVector<S>>()->resize(numElements);

    // ComplexType cannot be resized the same.
    if constexpr (!std::is_same_v<K, ComplexType>) {
      mapVector->mapKeys()->as<FlatVector<K>>()->resize(numElements);
    } else {
      mapVector->mapKeys()->resize(numElements);
    }

    auto rawNulls = mapVector->mutableRawNulls();
    vector_size_t offset = 0;
    for (auto i = 0; i < numGroups; ++i) {
      char* group = groups[i];
      if (isNull(group)) {
        bits::setNull(rawNulls, i, true);
        mapVector->setOffsetAndSize(i, 0, 0);
      } else {
        clearNull(rawNulls, i);

        auto mapSize = value<AccumulatorType>(group)->extractValues(
            mapKeysPtr, mapValuesPtr, offset);
        mapVector->setOffsetAndSize(i, offset, mapSize);
        offset += mapSize;
      }
    }
  }

  void extractAccumulators(char** groups, int32_t numGroups, VectorPtr* result)
      override {
    extractValues(groups, numGroups, result);
  }

  void addRawInput(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /*mayPushdown*/) override {
    decodedMaps_.decode(*args[0], rows);
    auto mapVector = decodedMaps_.base()->template as<MapVector>();
    auto mapKeys = mapVector->mapKeys();
    auto mapValues = mapVector->mapValues();

    rows.applyToSelected([&](auto row) {
      if (!decodedMaps_.isNullAt(row)) {
        auto* group = groups[row];
        clearNull(group);

        auto tracker = trackRowSize(group);
        auto groupMap = value<AccumulatorType>(group);
        addMap(*groupMap, mapVector, mapKeys, mapValues, row);
      }
    });
  }

  void addSingleGroupRawInput(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /* mayPushdown */) override {
    decodedMaps_.decode(*args[0], rows);
    auto mapVector = decodedMaps_.base()->template as<MapVector>();
    auto mapKeys = mapVector->mapKeys();
    auto mapValues = mapVector->mapValues();

    auto groupMap = value<AccumulatorType>(group);

    auto tracker = trackRowSize(group);
    rows.applyToSelected([&](auto row) {
      if (!decodedMaps_.isNullAt(row)) {
        clearNull(group);
        addMap(*groupMap, mapVector, mapKeys, mapValues, row);
      }
    });
  }

  void addIntermediateResults(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /*mayPushdown*/) override {
    addRawInput(groups, rows, args, false);
  }

  void addSingleGroupIntermediateResults(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /* mayPushdown */) override {
    addSingleGroupRawInput(group, rows, args, false);
  }

 protected:
  void initializeNewGroupsInternal(
      char** groups,
      folly::Range<const vector_size_t*> indices) override {
    setAllNulls(groups, indices);
    for (auto index : indices) {
      new (groups[index] + offset_) AccumulatorType{resultType_, allocator_};
    }
  }

  void destroyInternal(folly::Range<char**> groups) override {
    if constexpr (std::is_same_v<K, StringView>) {
      for (auto* group : groups) {
        if (isInitialized(group) && !isNull(group)) {
          value<AccumulatorType>(group)->strings.free(*allocator_);
        }
      }
    } else if constexpr (std::is_same_v<K, ComplexType>) {
      for (auto* group : groups) {
        if (isInitialized(group) && !isNull(group)) {
          value<AccumulatorType>(group)->free(*allocator_);
        }
      }
    }
    destroyAccumulators<AccumulatorType>(groups);
  }

 private:
  void addMap(
      AccumulatorType& groupMap,
      const MapVector* mapVector,
      const VectorPtr& mapKeys,
      const VectorPtr& mapValues,
      vector_size_t row) const {
    auto decodedRow = decodedMaps_.index(row);
    groupMap.addValues(mapVector, mapKeys, mapValues, decodedRow, allocator_);
  }

  vector_size_t countElements(char** groups, int32_t numGroups) const {
    vector_size_t size = 0;
    for (int32_t i = 0; i < numGroups; ++i) {
      size += value<AccumulatorType>(groups[i])->size();
    }
    return size;
  }

  DecodedVector decodedMaps_;
};

template <typename K>
std::unique_ptr<exec::Aggregate> createMapUnionSumAggregate(
    TypeKind valueKind,
    const TypePtr& resultType) {
  switch (valueKind) {
    case TypeKind::TINYINT:
      return std::make_unique<MapUnionSumAggregate<K, int8_t>>(resultType);
    case TypeKind::SMALLINT:
      return std::make_unique<MapUnionSumAggregate<K, int16_t>>(resultType);
    case TypeKind::INTEGER:
      return std::make_unique<MapUnionSumAggregate<K, int32_t>>(resultType);
    case TypeKind::BIGINT:
      return std::make_unique<MapUnionSumAggregate<K, int64_t>>(resultType);
    case TypeKind::REAL:
      return std::make_unique<MapUnionSumAggregate<K, float>>(resultType);
    case TypeKind::DOUBLE:
      return std::make_unique<MapUnionSumAggregate<K, double>>(resultType);
    default:
      VELOX_UNREACHABLE(
          "Unexpected value type {}", mapTypeKindToName(valueKind));
  }
}

} // namespace

void registerMapUnionSumAggregate(
    const std::string& prefix,
    bool withCompanionFunctions,
    bool overwrite) {
  const std::vector<std::string> valueTypes = {
      "tinyint", "smallint", "integer", "bigint", "double", "real"};

  // Add all allowed signatures.
  std::vector<std::shared_ptr<exec::AggregateFunctionSignature>> signatures;
  for (auto valueType : valueTypes) {
    signatures.push_back(
        exec::AggregateFunctionSignatureBuilder()
            .typeVariable("K")
            .returnType(fmt::format("map(K,{})", valueType))
            .intermediateType(fmt::format("map(K,{})", valueType))
            .argumentType(fmt::format("map(K,{})", valueType))
            .build());
  }

  auto name = prefix + kMapUnionSum;
  exec::registerAggregateFunction(
      name,
      std::move(signatures),
      [name](
          core::AggregationNode::Step /*step*/,
          const std::vector<TypePtr>& argTypes,
          const TypePtr& resultType,
          const core::QueryConfig& /*config*/)
          -> std::unique_ptr<exec::Aggregate> {
        VELOX_CHECK_EQ(argTypes.size(), 1);
        VELOX_CHECK(argTypes[0]->isMap());
        auto& mapType = argTypes[0]->asMap();
        auto keyTypeKind = mapType.keyType()->kind();
        auto valueTypeKind = mapType.valueType()->kind();

        switch (keyTypeKind) {
          case TypeKind::BOOLEAN:
            return createMapUnionSumAggregate<bool>(valueTypeKind, resultType);
          case TypeKind::TINYINT:
            return createMapUnionSumAggregate<int8_t>(
                valueTypeKind, resultType);
          case TypeKind::SMALLINT:
            return createMapUnionSumAggregate<int16_t>(
                valueTypeKind, resultType);
          case TypeKind::INTEGER:
            return createMapUnionSumAggregate<int32_t>(
                valueTypeKind, resultType);
          case TypeKind::BIGINT:
            return createMapUnionSumAggregate<int64_t>(
                valueTypeKind, resultType);
          case TypeKind::REAL:
            return createMapUnionSumAggregate<float>(valueTypeKind, resultType);
          case TypeKind::DOUBLE:
            return createMapUnionSumAggregate<double>(
                valueTypeKind, resultType);
          case TypeKind::TIMESTAMP:
            return createMapUnionSumAggregate<Timestamp>(
                valueTypeKind, resultType);
          case TypeKind::VARBINARY:
          case TypeKind::VARCHAR:
            return createMapUnionSumAggregate<StringView>(
                valueTypeKind, resultType);
          case TypeKind::ARRAY:
          case TypeKind::MAP:
          case TypeKind::ROW:
            return createMapUnionSumAggregate<ComplexType>(
                valueTypeKind, resultType);
          case TypeKind::UNKNOWN:
            return createMapUnionSumAggregate<UnknownValue>(
                valueTypeKind, resultType);
          default:
            if (mapType.keyType()->isDecimal()) {
              return createMapUnionSumAggregate<int128_t>(
                  valueTypeKind, resultType);
            }
            VELOX_UNREACHABLE(
                "Unexpected key type {}", mapTypeKindToName(keyTypeKind));
        }
      },
      withCompanionFunctions,
      overwrite);
}

} // namespace facebook::velox::aggregate::prestosql
