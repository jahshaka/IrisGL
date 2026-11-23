/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_MAPVALUE_H
#define IRIS_MAPVALUE_H

namespace iris
{

/// QHash::value(key) / QMap::value(key)'s answer for a std map (NOQT-DOCUMENT-1 stage 5): the mapped value, or `absent`
/// (a default-constructed value: a null pointer) for a missing key. A copy, never a reference into the map.
template <typename Map, typename Key>
typename Map::mapped_type valueOr(const Map &map, const Key &key,
                                  const typename Map::mapped_type &absent = typename Map::mapped_type())
{
    const auto it = map.find(key);
    return it == map.end() ? absent : it->second;
}

}

#endif // IRIS_MAPVALUE_H
