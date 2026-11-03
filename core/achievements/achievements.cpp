/*
	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
// Derived from duckstation: https://github.com/stenzek/duckstation/blob/master/src/core/achievements.cpp
// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)
#include "achievements.h"
#include "serialize.h"
// Deadcast: no achievements: a save state keeps the empty block it has always had

namespace achievements
{

void serialize(Serializer& ser)
{
	u32 size = 0;
	ser << size;
}

void deserialize(Deserializer& deser)
{
	if (deser.version() >= Deserializer::V50)
	{
		u32 size;
		deser >> size;
		deser.skip(size);
	}
}

}
