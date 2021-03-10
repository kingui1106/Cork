﻿// +-------------------------------------------------------------------------
// | CorkDefs.h
// | 
// | Author: Stephan Friedl
// +-------------------------------------------------------------------------
// | COPYRIGHT:
// |    Copyright Stephan Friedl 2015
// |    See the included COPYRIGHT file for further details.
// |    
// |    This file is part of the Cork library.
// |
// |    Cork is free software: you can redistribute it and/or modify
// |    it under the terms of the GNU Lesser General Public License as
// |    published by the Free Software Foundation, either version 3 of
// |    the License, or (at your option) any later version.
// |
// |    Cork is distributed in the hope that it will be useful,
// |    but WITHOUT ANY WARRANTY; without even the implied warranty of
// |    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// |    GNU Lesser General Public License for more details.
// |
// |    You should have received a copy 
// |    of the GNU Lesser General Public License
// |    along with Cork.  If not, see <http://www.gnu.org/licenses/>.
// +-------------------------------------------------------------------------

#pragma once


// NOTE: none of these values should be modified by the clients

const int QUANTIZATION_BITS = 30;							//	number of bits to set resolution of quantizing grid snapped to model
const int PERTURBATION_BUFFER_BITS = 0;						//	minimum number of bits of resolution that we will reserve when perturbing the model
															//		8 bits means that we will not perturb the mesh more than 1/128th of the length of the smallest edge in the model
const int PERTURBATION_RANGE_BITS = 5;						//	number of bits we will perturb over
const int MINIMUM_PERTURBATION_RANGE_BITS = 0;				//	initially perturbation range will be 2^2 or 4

//	If this is a 32 bit build, limit the number of points in the disjoint union to one million.  Beyond that and we run out of memory.
//		For 64 bit builds, go up to 5 million.

#if defined( _WIN32 ) && !defined( _WIN64 )
	#define MAX_TRIANGLES_IN_DISJOINT_UNION		2000000
#endif

#ifdef _WIN64
	#define MAX_TRIANGLES_IN_DISJOINT_UNION		5000000
#endif

//
//	Default to double precision, unless CORK_FLOAT_PRECISION is defined
//
//	Also, if __CORK_AVX__ is defined then use AVX intrinsics with floating point arithemtic.  AVX doesn't appear to speed up
//		the algorithm much but it is at least it is in there...
//


#ifdef CORK_FLOAT_PRECISION

	#define NUMERIC_PRECISION float

	#define NUMERIC_PRECISION_MAX					FLT_MAX
	#define NUMERIC_PRECISION_MIN					FLT_MIN
	#define NUMERIC_PRECISION_EPSILON				FLT_EPSILON
	#define NUMERIC_PRECISION_MIN_EPSILON			10 * FLT_EPSILON

	#define PURTURBATION_ORDERS_OF_MAG_LESS			4

	#define PURTURBATION_UNDERFLOW_EDGE_LENGTH		10 * NUMERIC_PRECISION_MIN_EPSILON

#else

	#define NUMERIC_PRECISION double

	#define NUMERIC_PRECISION_MAX					DBL_MAX
	#define NUMERIC_PRECISION_MIN					DBL_MIN
	#define NUMERIC_PRECISION_EPSILON				DBL_EPSILON
	#define NUMERIC_PRECISION_MIN_EPSILON			100 * DBL_EPSILON

	#define PURTURBATION_ORDERS_OF_MAG_LESS			5

	#define PURTURBATION_UNDERFLOW_EDGE_LENGTH		100 * NUMERIC_PRECISION_MIN_EPSILON

#endif



