#include "taco/storage/pack.h"

#include "taco/format.h"
#include "taco/error.h"
#include "ir/ir.h"
#include "taco/storage/storage.h"
#include "taco/util/collections.h"

using namespace std;

namespace taco {
namespace storage {

/// Count unique entries (assumes the values are sorted)
static vector<size_t> getUniqueEntries(const vector<int>::const_iterator& begin,
                                       const vector<int>::const_iterator& end) {
  vector<size_t> uniqueEntries;
  if (begin != end) {
    size_t curr = *begin;
    uniqueEntries.push_back(curr);
    for (auto it = begin+1; it != end; ++it) {
      size_t next = *it;
      taco_iassert(next >= curr);
      if (curr < next) {
        curr = next;
        uniqueEntries.push_back(curr);
      }
    }
  }
  return uniqueEntries;
}

#define PACK_NEXT_LEVEL(cend) { \
    if (i + 1 == dimTypes.size()) { \
      values->push_back((cbegin < cend) ? vals[cbegin] : 0.0); \
    } else { \
      packTensor(dims, coords, vals, cbegin, (cend), dimTypes, i+1, \
                 indices, values); \
    } \
}

/// Pack tensor coordinates into an index structure and value array.  The
/// indices consist of one index per tensor dimension, and each index contains
/// [0,2] index arrays.
static void packTensor(const vector<int>& dims,
                       const vector<vector<int>>& coords,
                       const double* vals,
                       size_t begin, size_t end,
                       const vector<DimensionType>& dimTypes, size_t i,
                       std::vector<std::vector<std::vector<int>>>* indices,
                       vector<double>* values) {
  auto& dimType     = dimTypes[i];
  auto& levelCoords = coords[i];
  auto& index       = (*indices)[i];

  switch (dimType) {
    case Dense: {
      // Iterate over each index value and recursively pack it's segment
      size_t cbegin = begin;
      for (int j=0; j < (int)dims[i]; ++j) {
        // Scan to find segment range of children
        size_t cend = cbegin;
        while (cend < end && levelCoords[cend] == j) {
          cend++;
        }
        PACK_NEXT_LEVEL(cend);
        cbegin = cend;
      }
      break;
    }
    case Sparse: {
      auto indexValues = getUniqueEntries(levelCoords.begin()+begin,
                                          levelCoords.begin()+end);

      // Store segment end: the size of the stored segment is the number of
      // unique values in the coordinate list
      index[0].push_back((int)(index[1].size() + indexValues.size()));

      // Store unique index values for this segment
      index[1].insert(index[1].end(), indexValues.begin(), indexValues.end());

      // Iterate over each index value and recursively pack it's segment
      size_t cbegin = begin;
      for (size_t j : indexValues) {
        // Scan to find segment range of children
        size_t cend = cbegin;
        while (cend < end && levelCoords[cend] == (int)j) {
          cend++;
        }
        PACK_NEXT_LEVEL(cend);
        cbegin = cend;
      }
      break;
    }
    case Fixed: {
      int fixedValue = index[0][0];
      auto indexValues = getUniqueEntries(levelCoords.begin()+begin,
                                          levelCoords.begin()+end);

      // Store segment end: the size of the stored segment is the number of
      // unique values in the coordinate list
      int segmentSize = indexValues.size() ;
      // Store unique index values for this segment
      size_t cbegin = begin;
      if (segmentSize > 0) {
        index[1].insert(index[1].end(), indexValues.begin(), indexValues.end());
        for (size_t j : indexValues) {
          // Scan to find segment range of children
          size_t cend = cbegin;
          while (cend < end && levelCoords[cend] == (int)j) {
            cend++;
          }
          PACK_NEXT_LEVEL(cend);
          cbegin = cend;
        }
      }
      // Complete index if necessary with the last index value
      auto curSize=segmentSize;
      while (curSize < fixedValue) {
        index[1].insert(index[1].end(), 
                        (segmentSize > 0) ? indexValues[segmentSize-1] : 0);
        PACK_NEXT_LEVEL(cbegin);
        curSize++;
      }
      break;
    }
  }
}

static int findMaxFixedValue(const vector<int>& dims,
                             const vector<vector<int>>& coords,
                             size_t order,
                             const size_t fixedLevel,
                             const size_t i, const size_t numCoords) {
  if (i == order) {
    return numCoords;
  }
  if (i == fixedLevel) {
    auto indexValues = getUniqueEntries(coords[i].begin(), coords[i].end());
    return indexValues.size();
  }
  else {
    // Find max occurrences for level i
    size_t maxSize=0;
    vector<int> maxCoords;
    int coordCur=coords[i][0];
    size_t sizeCur=1;
    for (size_t j=1; j<numCoords; j++) {
      if (coords[i][j] == coordCur) {
        sizeCur++;
      }
      else {
        if (sizeCur > maxSize) {
          maxSize = sizeCur;
          maxCoords.clear();
          maxCoords.push_back(coordCur);
        }
        else if (sizeCur == maxSize) {
          maxCoords.push_back(coordCur);
        }
        sizeCur=1;
        coordCur=coords[i][j];
      }
    }
    if (sizeCur > maxSize) {
      maxSize = sizeCur;
      maxCoords.clear();
      maxCoords.push_back(coordCur);
    }
    else if (sizeCur == maxSize) {
      maxCoords.push_back(coordCur);
    }

    int maxFixedValue=0;
    int maxSegment;
    vector<vector<int>> newCoords(order);
    for (size_t l=0; l<maxCoords.size(); l++) {
      // clean coords for next level
      for (size_t k=0; k<order;k++) {
        newCoords[k].clear();
      }
      for (size_t j=0; j<numCoords; j++) {
        if (coords[i][j] == maxCoords[l]) {
          for (size_t k=0; k<order;k++) {
            newCoords[k].push_back(coords[k][j]);
          }
        }
      }
      maxSegment = findMaxFixedValue(dims, newCoords, order, fixedLevel,
                                     i+1, maxSize);
      maxFixedValue = std::max(maxFixedValue,maxSegment);
    }
    return maxFixedValue;
  }
}

Storage pack(const std::vector<int>&              dimensions,
             const Format&                        format,
             const std::vector<std::vector<int>>& coordinates,
             const std::vector<double>            values) {
  taco_iassert(dimensions.size() == format.getOrder());

  Storage storage(format);

  size_t numDimensions = dimensions.size();
  size_t numCoordinates = values.size();

  // Create vectors to store pointers to indices/index sizes
  std::vector<std::vector<std::vector<int>>> indices;
  indices.reserve(numDimensions);

  for (size_t i=0; i < numDimensions; ++i) {
    switch (format.getDimensionTypes()[i]) {
      case Dense: {
        indices.push_back({});
        break;
      }
      case Sparse: {
        // Sparse indices have two arrays: a segment array and an index array
        indices.push_back({{}, {}});

        // Add start of first segment
        indices[i][0].push_back(0);
        break;
      }
      case Fixed: {
        // Fixed indices have two arrays: a segment array and an index array
        indices.push_back({{}, {}});

        // Add maximum size to segment array
        size_t maxSize = findMaxFixedValue(dimensions, coordinates,
                                           format.getOrder(), i, 0,
                                           numCoordinates);

        indices[i][0].push_back(maxSize);
        break;
      }
    }
  }

  std::vector<double> vals;
  packTensor(dimensions, coordinates, (const double*)values.data(), 0,
             numCoordinates, format.getDimensionTypes(), 0, &indices, &vals);

  // Copy packed data into tensor storage
  for (size_t i=0; i < numDimensions; ++i) {
    DimensionType dimensionType = format.getDimensionTypes()[i];

    switch (dimensionType) {
      case DimensionType::Dense: {
        auto size = util::copyToArray({dimensions[i]});
        storage.setDimensionIndex(i, {size});
        break;
      }
      case DimensionType::Sparse:
      case DimensionType::Fixed: {
        auto pos = util::copyToArray(indices[i][0]);
        auto idx = util::copyToArray(indices[i][1]);
        storage.setDimensionIndex(i, {pos,idx});
        break;
      }
    }
  }
  storage.setValues(util::copyToArray(vals));

  return storage;
}

ir::Stmt packCode(const Format& format) {
  ir::Stmt packStmt;

  // Generate loops to count the size of each level.
//  ir::Stmt countLoops;
//  for (auto& level : util::reverse(format.getLevels())) {
//    if (countLoops.defined()) {
//
//    }
//  }

  return packStmt;
}

}}
