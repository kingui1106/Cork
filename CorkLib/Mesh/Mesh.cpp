﻿
// +-------------------------------------------------------------------------
// | Mesh.cpp
// | 
// | Author: Gilbert Bernstein
// +-------------------------------------------------------------------------
// | COPYRIGHT:
// |    Copyright Gilbert Bernstein 2013
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


#include "..\Intersection\gmpext4.h"

#include <sstream>

#include "boost\container\small_vector.hpp"

#include "tbb/tbb.h"

#include "..\cork.h"

#include "..\Mesh\TopoCache.h"
#include "..\Mesh\IntersectionProblem.h"
#include "..\Mesh\EGraphCache.h"

#include "..\Intersection\unsafeRayTriIsct.h"

#include "..\Util\UnionFind.h"
#include "..\Util\SystemStats.h"

#include "..\Util\CachingFactory.h"

#include <boost\chrono\chrono.hpp>			//	Had to include when I upgraded to boost 65 - on windows is griped about a missing library
#include <boost\timer\timer.hpp>

#include "..\Util\ThreadPool.h"





Cork::EGraphCache::SkeletonColumnVectorFactory::CacheType			Cork::EGraphCache::SkeletonColumnVectorFactory::m_cache;

SEFUtility::CachingFactory<Cork::TopoCacheWorkspace>::CacheType		SEFUtility::CachingFactory<Cork::TopoCacheWorkspace>::m_cache;




namespace Cork
{

	using namespace Intersection;


	
	inline
	double triArea( const Cork::Math::Vector3D&		a,
					const Cork::Math::Vector3D&		b,
					const Cork::Math::Vector3D&		c )
	{
		return( len( cross( b-a, c-a )));
	}

	inline
	double triAreaSquared( const Cork::Math::Vector3D&		a,
						   const Cork::Math::Vector3D&		b,
						   const Cork::Math::Vector3D&		c )
	{
		return( len2( cross( b - a, c - a ) ) );
	}


		
	//
	//	The Mesh class brings together the functionality needed for the boolean operations
	//

	class Mesh : public MeshBase, public CorkMesh 
	{
	public:


		Mesh()
		{}

		Mesh( const Mesh&					src,
			  const SolverControlBlock		controlBlock )
			: MeshBase( src, controlBlock )
		{};

		explicit
		Mesh( const Cork::TriangleMesh&	inputMesh );

		virtual ~Mesh();
    

		void operator=(Mesh&&	src);
    

		//	Validity check:
		//		- all numbers are well-defined and finite
		//		- all triangle vertex indices are in the right range
 
		bool valid() const;
    

		// form the disjoint union of two meshes

		void							DisjointUnion( const Mesh&					meshToMerge );
    
		//	Boolean operations

		BooleanOperationResult			Union( const CorkMesh&								rhs,
											   const SolverControlBlock&					solverControlBlock = GetDefaultControlBlock() ) const;
		BooleanOperationResult			Difference( const CorkMesh&							rhs,
													const SolverControlBlock&				solverControlBlock = GetDefaultControlBlock() ) const;
		BooleanOperationResult			Intersection( const CorkMesh&						rhs,
													  const SolverControlBlock&				solverControlBlock = GetDefaultControlBlock() ) const;
		BooleanOperationResult			SymmetricDifference( const CorkMesh&				rhs,
															 const SolverControlBlock&		solverControlBlock = GetDefaultControlBlock() ) const;
    
		std::unique_ptr<TriangleMesh>	ToTriangleMesh() const;

		const SolverPerformanceStatisticsIfx&	GetPerformanceStats() const
		{
			return( m_performanceStats );
		}


		size_t			CountComponents() const;


	private:


		enum class TriCode { KEEP_TRI, DELETE_TRI, FLIP_TRI };


		enum class SetupBooleanProblemResultCodes { SUCCESS = 0,
													TOO_MANY_TRIANGLES_IN_DISJOINT_UNION,
													INSUFFICIENT_PERTURBATION_RANGE,
													FIND_INTERSECTIONS_FAILED,
													RESOLVE_INTERSECTIONS_FAILED,
													POPULATE_EDGE_GRAPH_CACHE_FAILED,
													SELF_INTERSECTING_MESH };

		typedef SEFUtility::Result<SetupBooleanProblemResultCodes>		SetupBooleanProblemResult;


		enum class BuildEGraphCacheResultCodes { SUCCESS = 0,
												 OUT_OF_MEMORY };

		typedef SEFUtility::ResultWithUniqueReturnPtr<BuildEGraphCacheResultCodes, EGraphCache>		BuildEGraphCacheResult;

		typedef tbb::concurrent_vector<size_t>				ComponentType;
		typedef tbb::concurrent_vector<ComponentType>		ComponentList;


		SetupBooleanProblemResult		SetupBooleanProblem( const Mesh&										rhs );

		BuildEGraphCacheResult			BuildEdgeGraphCache() const;

		std::unique_ptr<ComponentList>	FindComponents( EGraphCache&			ecache ) const;

		void							ProcessComponent( const EGraphCache&									ecache,
														  const ComponentType&									trisInComponent );

		size_t							FindTriForInsideTest( const ComponentType&								trisInComponent );

		void							doDeleteAndFlip( std::function<TriCode(byte bool_alg_data)>				classify );

		void							for_ecache( EGraphCache&												ecache,
													int															numThreads,
													std::function<void(const EGraphEntryTIDVector&	tids)>		action) const;
	
		bool							isInside( IndexType														tid,
												  byte															operand );

		void							RayTriangleIntersection( const CorkTriangle&							tri,
																 Cork::Math::Ray3D&								ray,
																 long&											winding );
	};





	inline void Mesh::for_ecache( EGraphCache&												ecache,
								  int														numThreads,
								  std::function<void(const EGraphEntryTIDVector&	tids)>	action ) const
	{
//		ThreadPool::getPool().parallel_for(numThreads, ecache.columns().begin(), ecache.columns().end(), [&](BlockRange<EGraphCache::SkeletonColumnVector::iterator>	partitionedCloumns )
//		tbb::parallel_for(tbb::blocked_range<EGraphCache::SkeletonColumnVector::iterator>(ecache.columns().begin(), ecache.columns().end(), ecache.columns().size() / 4 ),
//			[&](tbb::blocked_range<EGraphCache::SkeletonColumnVector::iterator> partitionedColumns)
		{
			for (auto& column : ecache.columns())
			{
				column.for_each([this, &action](EGraphEntry&	entry)
				{
					if (entry.isIsct())
					{
						EGraphEntryTIDVector		tid0s;
						EGraphEntryTIDVector		tid1s;

						for (IndexType tid : entry.tids())
						{
							if (m_tris[tid].boolAlgData() & 1)
							{
								tid1s.push_back(tid);
							}
							else
							{
								tid0s.push_back(tid);
							}
						}

						action(tid1s);
						action(tid0s);
					}
					else
					{
						action(entry.tids());
					}
				});
			}
		}
	}


	inline
	bool	Mesh::isInside( IndexType									tid,
							byte										operand )
	{
		// find the point to trace outward from...
    
		Cork::Math::Vector3D		p(m_verts[m_tris[tid].a()]);

		p += m_verts[m_tris[tid].b()];
		p += m_verts[m_tris[tid].c()];
		p /= 3.0;
        
		// ok, we've got the point, now let's pick a direction
        
		Cork::Math::Ray3DWithInverseDirection		directionRay( p, Cork::Math::Vector3D::randomVector( 0.5, 1.5 ) );

		long	winding = 0;

		//	Pass all triangles over ray
		//		Check for intersections with the triangle's bounding box to cull before the more
		//		expensive ray triangle intersection test.

		for( auto& tri : m_tris )
		{
			// ignore triangles from the same operand surface

			if( ( tri.boolAlgData() & 1 ) == operand )
			{
				continue;
			}

			//	Check the bounding box intersection first

			Cork::Math::BBox3D		boundingBox( min( m_verts[tri.a()], m_verts[tri.b()], m_verts[tri.c()] ), max( m_verts[tri.a()], m_verts[tri.b()], m_verts[tri.c()] ) );

			if( !boundingBox.intersects( directionRay ))
			{
				continue;
			}

			//	OK, we may have a hit so check for ray triangle intersection

			RayTriangleIntersection( tri, directionRay, winding );
		}

		// now, we've got a winding number to work with...
		return( winding > 0 );
	}



	inline
	void	Mesh::RayTriangleIntersection( const CorkTriangle&			tri,
										   Cork::Math::Ray3D&			r,
										   long&						winding )
	{
		NUMERIC_PRECISION flip = 1.0;

		IndexType   a = tri.a();
		IndexType   b = tri.b();
		IndexType   c = tri.c();

		Cork::Math::Vector3D		va = m_verts[a];
		Cork::Math::Vector3D		vb = m_verts[b];
		Cork::Math::Vector3D		vc = m_verts[c];

		// normalize vertex order (to prevent leaks)

		if( a > b )
		{
			std::swap( a, b ); std::swap( va, vb ); flip = -flip;
		}

		if( b > c )
		{
			std::swap( b, c ); std::swap( vb, vc ); flip = -flip;
		}

		if( a > b )
		{
			std::swap( a, b ); std::swap( va, vb ); flip = -flip;
		}

		if( CheckForRayTriangleIntersection( r, va, vb, vc ) )
		{
			Cork::Math::Vector3D	normal = flip * cross( vb - va, vc - va );

			if( dot( normal, r.direction() ) > 0.0 )
			{
				winding++;
			}
			else
			{
				winding--;
			}
		}
	}


	//
	//	Constructors and assignment operators
	//


	Mesh::Mesh( const Cork::TriangleMesh&		inputMesh )
	{
		m_tris.reserve( inputMesh.numTriangles() );
		m_verts.reserve( inputMesh.numVertices() );
	
		//	Start by copying the vertices.

		for ( Cork::TriangleMesh::Vertex currentVertex : inputMesh.vertices() )
		{
			m_verts.emplace_back( currentVertex.x(), currentVertex.y(), currentVertex.z() );
		}

		//	Fill the triangles

		for ( uint i = 0; i < inputMesh.triangles().size(); i++ )
		{
			m_tris.emplace_back( inputMesh.triangles()[i], 0 );
		}

		m_boundingBox = make_aligned<Cork::Math::BBox3D>( inputMesh.boundingBox() );
	}



	Mesh::~Mesh()
	{}




	void Mesh::operator=(Mesh &&src)
	{
		m_tris = src.m_tris;
		m_verts = src.m_verts;
	}


	bool Mesh::valid() const
	{
		for( unsigned int i=0; i< m_verts.size(); i++)
		{
			if(!std::isfinite(m_verts[i].x()) ||
			   !std::isfinite(m_verts[i].y()) ||
			   !std::isfinite(m_verts[i].z()))
			{
				std::ostringstream message;
				message << "vertex #" << i << " has non-finite coordinates: " << m_verts[i];
				CORK_ERROR(message.str());
				return false;
			}
		}
    
		for( unsigned int i=0; i<m_tris.size(); i++)
		{
			if(m_tris[i].a() >= m_verts.size() ||
			   m_tris[i].b() >= m_verts.size() ||
			   m_tris[i].c() >= m_verts.size())
			{
				std::ostringstream message;
				message << "triangle #" << i << " should have indices in "
						<< "the range 0 to " << (m_verts.size()-1)
						<< ", but it has invalid indices: "
						<< m_tris[i].a() << ", " << m_tris[i].b() << ", " << m_tris[i].c();
				CORK_ERROR(message.str());
				return false;
			}
		}
    
		return( true );
	}



	void Mesh::DisjointUnion( const Mesh&		meshToMerge )
	{
		//	Reset the labels on this mesh's collection of triangles

		for( auto& t : m_tris )
		{
			t.boolAlgData() = 0;
		}
	
		size_t		oldVsize = m_verts.size();
		size_t		oldTsize = m_tris.size();
		size_t		cpVsize  = meshToMerge.m_verts.size();
		size_t		cpTsize  = meshToMerge.m_tris.size();
		size_t		newVsize = oldVsize + cpVsize;
		size_t		newTsize = oldTsize + cpTsize;
    
		m_verts.resize( newVsize );
		m_tris.resize( newTsize );
    
		for( unsigned int i = 0; i < cpVsize; i++ )
		{
			m_verts[oldVsize + i] = meshToMerge.m_verts[i];
		}

		for( unsigned int i = 0; i < cpTsize; i++ )
		{
			auto &tri = m_tris[oldTsize + i];

			tri = meshToMerge.m_tris[i];
			tri.boolAlgData() = 1;					//	These triangles are part of the RHS so label them as such
			tri.offsetIndices( oldVsize );
		}
	}




	Mesh::SetupBooleanProblemResult		Mesh::SetupBooleanProblem( const Mesh&		rhs )
	{
		auto		intersectionBBox = m_boundingBox->intersection( rhs.boundingBox() );
	
		//	Form the disjoint union of this mesh and the second operand mesh
		
		DisjointUnion( rhs );

		m_performanceStats.setNumberOfTrianglesInDisjointUnion( (unsigned long)this->m_tris.size() );
		m_controlBlock->setNumTriangles( (unsigned long)this->m_tris.size() );

		if( this->m_tris.size() >= MAX_TRIANGLES_IN_DISJOINT_UNION )
		{
			return( SetupBooleanProblemResult::Failure( SetupBooleanProblemResultCodes::TOO_MANY_TRIANGLES_IN_DISJOINT_UNION, "Too many triangles in disjoint union, possible out of memory exception if the operation is attenmpted." ));
		}

		//	Start by finding the intersections

		int		tries = 5;
		
		Quantization::Quantizer			quantizer = getQuantizer();

		if (!quantizer.sufficientPerturbationRange())
		{
			return(SetupBooleanProblemResult::Failure(SetupBooleanProblemResultCodes::INSUFFICIENT_PERTURBATION_RANGE, "Insufficient Dynamic Range left in model for perturbation."));
		}

		//	Find intersections and then resolve them.  We might have to repurturb if finding and resolving fails.
		//		We can repurturb until we run out of perturbation resolution.

		std::unique_ptr<IntersectionProblemIfx>					iproblem( IntersectionProblemIfx::GetProblem( *this, quantizer, intersectionBBox ) );

		while( true )
		{
			IntersectionProblemIfx::IntersectionProblemResult		findResult = iproblem->FindIntersections();

			if( !findResult.Succeeded() )
			{
				//	If we failed here - not mush to do but return a failed result

				return( SetupBooleanProblemResult::Failure( SetupBooleanProblemResultCodes::FIND_INTERSECTIONS_FAILED, "FindIntersections failed.", findResult ));
			}

			//	Next, resolve them

			IntersectionProblemIfx::IntersectionProblemResult		resolveResult = iproblem->ResolveAllIntersections();

			if( !resolveResult.Succeeded() )
			{
				//	Resolve failed, check the error code to see if this is a recoverable error or not

				//	If we failed due to a self-intersection, then one of the meshes is bad so no amount of repurturbation will work.

				if( resolveResult.errorCode() == IntersectionProblemIfx::IntersectionProblemResultCodes::SELF_INTERSECTING_MESH )
				{
					return( SetupBooleanProblemResult::Failure( SetupBooleanProblemResultCodes::SELF_INTERSECTING_MESH, "One of the two meshes self intersects", resolveResult ) );
				}

				//	Resolve failed for some other reason.

				return( SetupBooleanProblemResult::Failure( SetupBooleanProblemResultCodes::RESOLVE_INTERSECTIONS_FAILED, "ResolveIntersections failed and exhuasted perturbations.", resolveResult ) );
			}
    
			iproblem->commit();
			break;
		}

		//	Create and populate the EGraph Cache

		BuildEGraphCacheResult					buildEGraphResult = BuildEdgeGraphCache();

		if( !buildEGraphResult.Succeeded() )
		{
			return( SetupBooleanProblemResult::Failure( SetupBooleanProblemResultCodes::POPULATE_EDGE_GRAPH_CACHE_FAILED, "Building Edge Graph Cache Failed", buildEGraphResult ));
		}

		std::unique_ptr<EGraphCache>			ecache( std::move( buildEGraphResult.ReturnPtr() ));

		// form connected components;
		// we get one component for each connected component in one
		// of the two input meshes.
		// These components are not necessarily uniformly inside or outside
		// of the other operand mesh.

		std::unique_ptr<ComponentList>		components( std::move( FindComponents( *ecache ) ) );

		if( solverControlBlock().useMultipleThreads() && ( components->size() > 1 ))
		{
			size_t partitionSize = 1;

			if( components->size() > 8 )
			{
				partitionSize = components->size() / 8;
			}

			tbb::parallel_for( tbb::blocked_range<ComponentList::iterator>( components->begin(), components->end(), partitionSize ),
				[&]( tbb::blocked_range<ComponentList::iterator> partitionedComponents )
			{
				for( auto&	comp : partitionedComponents )
				{
					ProcessComponent( *ecache, comp );
				}
			}, tbb::simple_partitioner() );
		}
		else
		{
			for( auto& comp : *components )
			{
				ProcessComponent( *ecache, comp );
			}
		}

		//	Finished with Success

		return( SetupBooleanProblemResult::Success() );
	}


	Mesh::BuildEGraphCacheResult		Mesh::BuildEdgeGraphCache() const
	{
		std::unique_ptr<EGraphCache>				ecachePtr( new EGraphCache );

		try
		{
			ecachePtr->resize( m_verts.size() );
		}
		catch( std::bad_alloc&		ex )
		{
			return( BuildEGraphCacheResult::Failure( BuildEGraphCacheResultCodes::OUT_OF_MEMORY, "Out of Memory resizing the edge cache" ) );
		}


		EGraphCache&		ecache = *ecachePtr;


		for( uint tid = 0; tid < m_tris.size(); tid++ )
		{
			const CorkTriangle&		tri = m_tris[tid];

			ecache[tri.a()].find_or_add( tri.b() ).tids().push_back( tid );
			ecache[tri.a()].find_or_add( tri.c() ).tids().push_back( tid );

			ecache[tri.b()].find_or_add( tri.a() ).tids().push_back( tid );
			ecache[tri.b()].find_or_add( tri.c() ).tids().push_back( tid );

			ecache[tri.c()].find_or_add( tri.a() ).tids().push_back( tid );
			ecache[tri.c()].find_or_add( tri.b() ).tids().push_back( tid );
		}

		//	Label some of the edges as intersection edges and others as not

		for( auto& column : ecache.columns() )
		{
			column.for_each( [this] ( EGraphEntry &entry )
			{
				entry.setIsIsct( false );
				byte operand = m_tris[entry.tids()[0]].boolAlgData();

				for( uint k = 1; k<entry.tids().size(); k++ )
				{
					if( m_tris[entry.tids()[k]].boolAlgData() != operand )
					{
						entry.setIsIsct( true );
						break;
					}
				}
			} );
		}

		//	Finished with success

		return( BuildEGraphCacheResult::Success( ecachePtr.release() ) );
	}



	std::unique_ptr<Mesh::ComponentList>		Mesh::FindComponents( EGraphCache&			ecache ) const
	{
		// form connected components;
		// we get one component for each connected component in one
		// of the two input meshes.
		// These components are not necessarily uniformly inside or outside
		// of the other operand mesh.

		RandomWeightedParallelUnionFind		uf(m_tris.size());

		for_ecache( ecache, 3, [&uf] ( const EGraphEntryTIDVector&	tids )
		{
			size_t		tid0 = tids[0];

			for( size_t k = 1; k < tids.size(); k++ )
			{
				uf.unite( tid0, tids[k] );
			}
		} );

		// we re-organize the results of the union find as follows:

		std::vector<long>						uq_ids( m_tris.size(), long( -1 ) );
		std::unique_ptr<ComponentList>			components( new ComponentList );

		components->reserve( 256 );

		std::mutex			vectorLock;

		size_t blockBegin = 0;
		size_t blockEnd = m_tris.size();	
		size_t		ufid;
		for (size_t i = blockBegin; i < blockEnd; i++)
		{
			ufid = uf.find(i);

			retry :

			if (uq_ids[ufid] == long(-1))
			{
				std::lock_guard<std::mutex>		lock(vectorLock);

				if (uq_ids[ufid] != long(-1))
					goto retry;

				size_t N = components->size();
				components->emplace_back();
//					(*components)[N].reserve(512);

				uq_ids[ufid] = uq_ids[i] = (long)N;
				(*components)[N].push_back(i);
			}
			else
			{
				uq_ids[i] = uq_ids[ufid];
				(*components)[uq_ids[i]].push_back(i);
			}
		}
		return( components );
	}


	size_t			Mesh::CountComponents() const
	{
		BuildEGraphCacheResult											ecacheResult = BuildEdgeGraphCache();

		std::unique_ptr<EGraphCache>									ecache( std::move( ecacheResult.ReturnPtr() ));

		std::unique_ptr<ComponentList>									components( std::move( FindComponents( *ecache ) ) );

		SEFUtility::CachingFactory<TopoCacheWorkspace>::UniquePtr		topoCacheWorkspace(SEFUtility::CachingFactory<TopoCacheWorkspace>::GetInstance() );


		std::vector<std::set<size_t>>		bodies;

		bodies.reserve( components->size() );

		for( int i = 0; i < components->size(); i++ )
		{
			ComponentType&			component = ( *components )[i];

			bodies.emplace_back();

			std::set<size_t>&				bodyByVerts = bodies.back();
			
			for( auto triIndex : component )
			{
				bodyByVerts.insert( m_tris[triIndex].a() );
				bodyByVerts.insert( m_tris[triIndex].b() );
				bodyByVerts.insert( m_tris[triIndex].c() );
			}
		}

		for( int i = 0; i < bodies.size(); i++ )
		{
			for( int j = i + 1; j < bodies.size(); j++ )
			{
				bool	merged = false;

				std::set<size_t>&			body1 = bodies[i];
				std::set<size_t>&			body2 = bodies[j];

				for( auto element2 : body2 )
				{
					if( body1.find( element2 ) != body1.end() )
					{
						body1.insert( body2.begin(), body2.end() );

						bodies.erase( bodies.begin() + j);

						j--;

						merged = true;
						break;
					}
				}

				if( merged )
				{
					i--;
					break;
				}
			}
		}

		return( bodies.size() );
	}



	void			Mesh::ProcessComponent( const EGraphCache&			ecache,
											const ComponentType&		trisInComponent )
	{
		// find the "best" triangle in each component,
		// and ray cast to determine inside-ness vs. outside-ness

		size_t			best_tid = FindTriForInsideTest( trisInComponent );

		//	Do the 'inside' test

		byte operand = m_tris[best_tid].boolAlgData();
		bool inside = isInside( best_tid, operand );

		//	Do a breadth first propagation of classification throughout the component.

		std::vector<size_t>	work;
		work.reserve( trisInComponent.size() );

		std::vector<bool> visited( m_tris.size(), false );

		// begin by tagging the first triangle

		m_tris[best_tid].boolAlgData() |= ( inside ) ? 2 : 0;
		visited[best_tid] = true;
		work.push_back( best_tid );

		while( !work.empty() )
		{
			size_t		curr_tid = work.back();
			work.pop_back();

			for( uint k = 0; k<3; k++ )
			{
				size_t		a = m_tris[curr_tid][k];
				size_t		b = m_tris[curr_tid][( k + 1 ) % 3];

				auto&	entry = ecache[a][b];

				byte inside_sig = m_tris[curr_tid].boolAlgData() & 2;

				if( entry.isIsct() )
				{
					inside_sig ^= 2;
				}

				for( size_t tid : entry.tids() )
				{
					if( visited[tid] )
					{
						continue;
					}

					if( ( m_tris[tid].boolAlgData() & 1 ) != operand )
					{
						continue;
					}

					m_tris[tid].boolAlgData() |= inside_sig;
					visited[tid] = true;
					work.push_back( tid );
				}
			}
		}
	}


	size_t			Mesh::FindTriForInsideTest( const ComponentType&			trisInComponent )
	{
		size_t			currentTid;
		size_t			best_tid = trisInComponent[0];
		double			best_area = 0.0;

		size_t			searchIncrement = 1;


		//	We will adjust the search increment based on the number of triangles to search.

		if( trisInComponent.size() > 1000 )
		{
			searchIncrement = 2;
		}

		if( trisInComponent.size() > 25000 )
		{
			searchIncrement = 3;
		}

		if( trisInComponent.size() > 50000 )
		{
			searchIncrement = 4;
		}

		if( trisInComponent.size() > 100000 )
		{
			searchIncrement = 5;
		}

		//	Do the search - we are looking for the triangle with the greatest surface area to use
		//		as the representative triangle to test 'insideness' of component.

		for( size_t i = 0; i < trisInComponent.size(); i += searchIncrement )
		{
			currentTid = trisInComponent[i];

			const Cork::Math::Vector3D&		va = m_verts[m_tris[currentTid].a()];
			const Cork::Math::Vector3D&		vb = m_verts[m_tris[currentTid].b()];
			const Cork::Math::Vector3D&		vc = m_verts[m_tris[currentTid].c()];

			double area = triAreaSquared( va, vb, vc );		//	We don't need the square root, we just want the biggest surface area

			if( area > best_area )
			{
				best_area = area;
				best_tid = currentTid;
			}
		}

		return( best_tid );
	}



	void Mesh::doDeleteAndFlip( std::function<TriCode(byte bool_alg_data)> classify )
	{
		SEFUtility::CachingFactory<TopoCacheWorkspace>::UniquePtr			topoCacheWorkspace(SEFUtility::CachingFactory<TopoCacheWorkspace>::GetInstance());
		
		TopoCache					topocache( *this, *topoCacheWorkspace );
    
		std::vector<TopoTri*>		toDelete;
    
		toDelete.reserve( topocache.triangles().size() / 2 );


		for( auto& currentTriangle : topocache.triangles() )
		{
			TriCode code = classify( m_tris[currentTriangle.ref()].boolAlgData() );

			switch(code)
			{
				case TriCode::DELETE_TRI:
					toDelete.push_back( &currentTriangle );
					break;
        
				case TriCode::FLIP_TRI:
					topocache.flipTri(  &currentTriangle );
					break;
        
				case TriCode::KEEP_TRI:
        
				default:
					break;
			}
		}
    
		for( auto tptr : toDelete)
		{
			topocache.deleteTri(tptr);
		}
    
		topocache.commit();
	}




	Mesh::BooleanOperationResult	Mesh::Union( const CorkMesh&					rhs,
												 const SolverControlBlock&			solverControlBlock ) const
	{
		//	Collect some starting statistics
		
		unsigned long				startingVirtualMemory = GetConsumedVirtualMemory();
		
		boost::timer::cpu_timer		elapsedTime;

		elapsedTime.start();

		//	Duplicate the mesh

		std::unique_ptr<Mesh>			resultMesh( new Mesh( *this, solverControlBlock ));

		resultMesh->m_performanceStats.setStartingVirtualMemorySizeInMB( startingVirtualMemory );

		SetupBooleanProblemResult		result = resultMesh->SetupBooleanProblem( dynamic_cast<const Cork::Mesh&>(rhs) );

		if( !result.Succeeded() )
		{
			return( BooleanOperationResult::Failure( BooleanOperationResultCodes::ERROR_DURING_BOOLEAN_PROBLEM_SETUP, "Error Occurred During Boolean Problem Setup Phase.", result ) );
		}

		resultMesh->doDeleteAndFlip([](byte data) -> TriCode
		{
			if((data & 2) == 2)     // part of op 0/1 INSIDE op 1/0
			{
				return TriCode::DELETE_TRI;
			}
			else                    // part of op 0/1 OUTSIDE op 1/0
			{
				return TriCode::KEEP_TRI;
			}
		});

		//	Collect the ending statistics

		elapsedTime.stop();
		resultMesh->m_performanceStats.setElapsedCPUTimeInNanoSeconds( elapsedTime.elapsed().system + elapsedTime.elapsed().user );
		resultMesh->m_performanceStats.setElapsedWallTimeInNanoSeconds( elapsedTime.elapsed().wall );

		resultMesh->m_performanceStats.setNumberOfTrianglesInFinalMesh( (unsigned long)resultMesh->triangles().size() );
		resultMesh->m_performanceStats.setEndingVirtualMemorySizeInMB( GetConsumedVirtualMemory() );

		//	Finished with success

		return( BooleanOperationResult::Success( resultMesh.release() ));
	}



	Mesh::BooleanOperationResult	Mesh::Difference( const CorkMesh&				rhs,
													  const SolverControlBlock&		solverControlBlock ) const
	{
		//	Collect some starting statistics
		
		unsigned long		startingVirtualMemory = GetConsumedVirtualMemory();
		
		boost::timer::cpu_timer		elapsedTime;

		elapsedTime.start();

		//	Duplicate the mesh

		std::unique_ptr<Mesh>			resultMesh( new Mesh( *this, solverControlBlock ) );

		resultMesh->m_performanceStats.setStartingVirtualMemorySizeInMB( startingVirtualMemory );

		SetupBooleanProblemResult		result = resultMesh->SetupBooleanProblem( dynamic_cast<const Cork::Mesh&>(rhs) );

		if( !result.Succeeded() )
		{
			return( BooleanOperationResult::Failure( BooleanOperationResultCodes::ERROR_DURING_BOOLEAN_PROBLEM_SETUP, "Error Occurred During Boolean Problem Setup Phase.", result ) );
		}
    
		resultMesh->doDeleteAndFlip([](byte data) -> TriCode
		{
			if(data == 2 || data == 1)      // part of op 0 INSIDE op 1, part of op 1 OUTSIDE op 0
			{
				return TriCode::DELETE_TRI;
			}
			else if(data == 3)      // part of op 1 INSIDE op 1
			{
				return TriCode::FLIP_TRI;
			}
			else                    // part of op 0 OUTSIDE op 1
			{
				return TriCode::KEEP_TRI;
			}
		});

		//	Collect the ending statistics

		elapsedTime.stop();
		resultMesh->m_performanceStats.setElapsedCPUTimeInNanoSeconds( elapsedTime.elapsed().system + elapsedTime.elapsed().user );
		resultMesh->m_performanceStats.setElapsedWallTimeInNanoSeconds( elapsedTime.elapsed().wall );

		resultMesh->m_performanceStats.setNumberOfTrianglesInFinalMesh( (unsigned long)resultMesh->triangles().size() );
		resultMesh->m_performanceStats.setEndingVirtualMemorySizeInMB( GetConsumedVirtualMemory() );

		//	Finished with success

		return( BooleanOperationResult::Success( resultMesh.release() ));
	}



	Mesh::BooleanOperationResult	Mesh::Intersection( const CorkMesh&					rhs,
														const SolverControlBlock&		solverControlBlock ) const
	{
		//	Collect some starting statistics
		
		unsigned long			startingVirtualMemory = GetConsumedVirtualMemory();
		
		boost::timer::cpu_timer		elapsedTime;

		elapsedTime.start();

		//	Duplicate the mesh

		std::unique_ptr<Mesh>			resultMesh( new Mesh( *this, solverControlBlock ) );

		resultMesh->m_performanceStats.setStartingVirtualMemorySizeInMB( startingVirtualMemory );

		SetupBooleanProblemResult		result = resultMesh->SetupBooleanProblem( dynamic_cast<const Cork::Mesh&>(rhs) );

		if( !result.Succeeded() )
		{
			return( BooleanOperationResult::Failure( BooleanOperationResultCodes::ERROR_DURING_BOOLEAN_PROBLEM_SETUP, "Error Occurred During Boolean Problem Setup Phase.", result ) );
		}

		//	Don't let the returns below confuse you - the code is a lambda

		resultMesh->doDeleteAndFlip([](byte data) -> TriCode
		{
			if((data & 2) == 0)     // part of op 0/1 OUTSIDE op 1/0
			{
				return( TriCode::DELETE_TRI );
			}
			else                    // part of op 0/1 INSIDE op 1/0
			{
				return( TriCode::KEEP_TRI );
			}
		});

		//	Collect the ending statistics

		elapsedTime.stop();
		resultMesh->m_performanceStats.setElapsedCPUTimeInNanoSeconds( elapsedTime.elapsed().system + elapsedTime.elapsed().user );
		resultMesh->m_performanceStats.setElapsedWallTimeInNanoSeconds( elapsedTime.elapsed().wall );

		resultMesh->m_performanceStats.setNumberOfTrianglesInFinalMesh( (unsigned long)resultMesh->triangles().size() );
		resultMesh->m_performanceStats.setEndingVirtualMemorySizeInMB( GetConsumedVirtualMemory() );

		//	Finished with success

		return( BooleanOperationResult::Success( resultMesh.release() ));
	}



	Mesh::BooleanOperationResult	Mesh::SymmetricDifference( const CorkMesh&					rhs,
															   const SolverControlBlock&		solverControlBlock ) const
	{
		//	Collect some starting statistics
		
		unsigned long			startingVirtualMemory = GetConsumedVirtualMemory();
		
		boost::timer::cpu_timer		elapsedTime;

		elapsedTime.start();

		//	Duplicate the mesh

		std::unique_ptr<Mesh>			resultMesh( new Mesh( *this, solverControlBlock ) );

		resultMesh->m_performanceStats.setStartingVirtualMemorySizeInMB( startingVirtualMemory );

		SetupBooleanProblemResult		result = resultMesh->SetupBooleanProblem( dynamic_cast<const Cork::Mesh&>(rhs) );

		if( !result.Succeeded() )
		{
			return( BooleanOperationResult::Failure( BooleanOperationResultCodes::ERROR_DURING_BOOLEAN_PROBLEM_SETUP, "Error Occurred During Boolean Problem Setup Phase.", result ) );
		}
    
		//	Don't let the returns below confuse you - the code is a lambda

		resultMesh->doDeleteAndFlip([](byte data) -> TriCode
		{
			if((data & 2) == 0 )     // part of op 0/1 OUTSIDE op 1/0
			{
				return( TriCode::KEEP_TRI );
			}
			else if((data & 2) == 2 )
			{
				return( TriCode::DELETE_TRI );
			}
			else
			{
				return( TriCode::FLIP_TRI );
			}
		});

		//	Collect the ending statistics

		elapsedTime.stop();
		resultMesh->m_performanceStats.setElapsedCPUTimeInNanoSeconds( elapsedTime.elapsed().system + elapsedTime.elapsed().user );
		resultMesh->m_performanceStats.setElapsedWallTimeInNanoSeconds( elapsedTime.elapsed().wall );

		resultMesh->m_performanceStats.setNumberOfTrianglesInFinalMesh( (unsigned long)resultMesh->triangles().size() );
		resultMesh->m_performanceStats.setEndingVirtualMemorySizeInMB( GetConsumedVirtualMemory() );

		//	Finished with success

		return( Mesh::BooleanOperationResult::Success( resultMesh.release() ));
	}

	
	std::unique_ptr<TriangleMesh>				Mesh::ToTriangleMesh() const
	{
		std::unique_ptr<Cork::IncrementalVertexIndexTriangleMeshBuilder>		triangleMeshBuilder( Cork::IncrementalVertexIndexTriangleMeshBuilder::GetBuilder( vertices().size(), triangles().size() ) );

		for( auto& currentVertex : vertices() )
		{
			triangleMeshBuilder->AddVertex(Cork::TriangleMesh::Vertex((NUMERIC_PRECISION)currentVertex.x(), (NUMERIC_PRECISION)currentVertex.y(), (NUMERIC_PRECISION)currentVertex.z()));
		}

		for_raw_tris( [&]( IndexType a, IndexType b, IndexType c )
		{
			triangleMeshBuilder->AddTriangle( TriangleMesh::TriangleByIndices( a, b, c ) );
		} );

		return(triangleMeshBuilder->Mesh());
	}


	
	std::unique_ptr<CorkMesh>			CorkMesh::FromTriangleMesh( const TriangleMesh&		triangleMesh )
	{
		return(std::unique_ptr<CorkMesh>( new Mesh( triangleMesh ) ));
	}


	const SolverControlBlock&					CorkMesh::GetDefaultControlBlock()
	{
		static	SolverControlBlock		defaultBlock( true, (long)50000, true );
		
		return( defaultBlock );
	}


//	SelfIntersectionStatistics			ComputeTriangleMeshSelfIntersectionStatistics( const Cork::TriangleMesh&		triangleMeshToTest )
//	{
//		Mesh			meshForTesting( triangleMeshToTest );
//
//		return( meshForTesting.ComputeSelfIntersectionStatistics() );
//	}

}	//	Namespace Cork
