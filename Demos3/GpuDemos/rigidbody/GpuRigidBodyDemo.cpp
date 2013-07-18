#include "GpuRigidBodyDemo.h"
#include "OpenGLWindow/ShapeData.h"
#include "OpenGLWindow/GLInstancingRenderer.h"
#include "Bullet3Common/b3Quaternion.h"
#include "OpenGLWindow/b3gWindowInterface.h"
#include "Bullet3OpenCL/BroadphaseCollision/b3GpuSapBroadphase.h"
#include "../GpuDemoInternalData.h"
#include "Bullet3OpenCL/Initialize/b3OpenCLUtils.h"
#include "OpenGLWindow/OpenGLInclude.h"
#include "OpenGLWindow/GLInstanceRendererInternalData.h"
#include "Bullet3OpenCL/ParallelPrimitives/b3LauncherCL.h"
#include "Bullet3OpenCL/RigidBody/b3GpuRigidBodyPipeline.h"
#include "Bullet3OpenCL/RigidBody/b3GpuNarrowPhase.h"
#include "Bullet3OpenCL/RigidBody/b3Config.h"
#include "GpuRigidBodyDemoInternalData.h"
#include "Bullet3Collision/BroadPhaseCollision/b3DynamicBvhBroadphase.h"
#include "Bullet3Collision/NarrowPhaseCollision/b3RigidBodyCL.h"

static b3KeyboardCallback oldCallback = 0;
extern bool gReset;

#define MSTRINGIFY(A) #A

static const char* s_rigidBodyKernelString = MSTRINGIFY(

typedef struct
{
	float4 m_pos;
	float4 m_quat;
	float4 m_linVel;
	float4 m_angVel;
	unsigned int m_collidableIdx;
	float m_invMass;
	float m_restituitionCoeff;
	float m_frictionCoeff;
} Body;

__kernel void 
	copyTransformsToVBOKernel( __global Body* gBodies, __global float4* posOrnColor, const int numNodes)
{
	int nodeID = get_global_id(0);
	if( nodeID < numNodes )
	{
		posOrnColor[nodeID] = (float4) (gBodies[nodeID].m_pos.xyz,1.0);
		posOrnColor[nodeID + numNodes] = gBodies[nodeID].m_quat;
	}
}
);





GpuRigidBodyDemo::GpuRigidBodyDemo()
:m_instancingRenderer(0),
m_window(0)
{
	m_data = new GpuRigidBodyDemoInternalData;
}
GpuRigidBodyDemo::~GpuRigidBodyDemo()
{
	
	delete m_data;
}







static void PairKeyboardCallback(int key, int state)
{
	if (key=='R' && state)
	{
		gReset = true;
	}
	
	//b3DefaultKeyboardCallback(key,state);
	oldCallback(key,state);
}

void	GpuRigidBodyDemo::setupScene(const ConstructionInfo& ci)
{

}

void	GpuRigidBodyDemo::initPhysics(const ConstructionInfo& ci)
{

	if (ci.m_window)
	{
		m_window = ci.m_window;
		oldCallback = ci.m_window->getKeyboardCallback();
		ci.m_window->setKeyboardCallback(PairKeyboardCallback);

	}

	m_instancingRenderer = ci.m_instancingRenderer;

	initCL(ci.preferredOpenCLDeviceIndex,ci.preferredOpenCLPlatformIndex);

	if (m_clData->m_clContext)
	{
		int errNum=0;

		cl_program rbProg=0;
		m_data->m_copyTransformsToVBOKernel = b3OpenCLUtils::compileCLKernelFromString(m_clData->m_clContext,m_clData->m_clDevice,s_rigidBodyKernelString,"copyTransformsToVBOKernel",&errNum,rbProg);
		
		b3Config config;
		config.m_maxConvexBodies = b3Max(config.m_maxConvexBodies,ci.arraySizeX*ci.arraySizeY*ci.arraySizeZ+10);
		config.m_maxConvexShapes = config.m_maxConvexBodies;
		config.m_maxBroadphasePairs = 16*config.m_maxConvexBodies;
		config.m_maxContactCapacity = config.m_maxBroadphasePairs;
		

		b3GpuNarrowPhase* np = new b3GpuNarrowPhase(m_clData->m_clContext,m_clData->m_clDevice,m_clData->m_clQueue,config);
		b3GpuSapBroadphase* bp = new b3GpuSapBroadphase(m_clData->m_clContext,m_clData->m_clDevice,m_clData->m_clQueue);
		m_data->m_np = np;
		m_data->m_bp = bp;
		m_data->m_broadphaseDbvt = new b3DynamicBvhBroadphase(config.m_maxConvexBodies);

		m_data->m_rigidBodyPipeline = new b3GpuRigidBodyPipeline(m_clData->m_clContext,m_clData->m_clDevice,m_clData->m_clQueue, np, bp,m_data->m_broadphaseDbvt,config);


		setupScene(ci);

		m_data->m_rigidBodyPipeline->writeAllInstancesToGpu();
		np->writeAllBodiesToGpu();
		bp->writeAabbsToGpu();

	}

	

	m_instancingRenderer->writeTransforms();
	
	

}

void	GpuRigidBodyDemo::exitPhysics()
{
	destroyScene();

	delete m_data->m_instancePosOrnColor;
	delete m_data->m_rigidBodyPipeline;
	delete m_data->m_broadphaseDbvt;

	m_window->setKeyboardCallback(oldCallback);
	
	delete m_data->m_np;
	m_data->m_np = 0;
	delete m_data->m_bp;
	m_data->m_bp = 0;

	exitCL();
}


void GpuRigidBodyDemo::renderScene()
{
	m_instancingRenderer->renderScene();
}

void GpuRigidBodyDemo::clientMoveAndDisplay()
{
	bool animate=true;
	int numObjects= m_data->m_rigidBodyPipeline->getNumBodies();
	//printf("numObjects=%d\n",numObjects);
	if (numObjects > m_instancingRenderer->getInstanceCapacity())
	{
		static bool once = true;
		if (once)
		{
			once=false;
			b3Assert(0);
			b3Error("m_instancingRenderer out-of-memory\n");
		}
		numObjects = m_instancingRenderer->getInstanceCapacity();
	}

	GLint err = glGetError();
			assert(err==GL_NO_ERROR);

	b3Vector4* positions = 0;
	if (animate && numObjects)
	{
		B3_PROFILE("gl2cl");
		
		if (!m_data->m_instancePosOrnColor)
		{
			GLuint vbo = m_instancingRenderer->getInternalData()->m_vbo;
			int arraySizeInBytes  = numObjects * (3)*sizeof(b3Vector4);
			glBindBuffer(GL_ARRAY_BUFFER, vbo);
			cl_bool blocking=  CL_TRUE;
			positions=  (b3Vector4*)glMapBufferRange( GL_ARRAY_BUFFER,m_instancingRenderer->getMaxShapeCapacity(),arraySizeInBytes, GL_MAP_READ_BIT );//GL_READ_WRITE);//GL_WRITE_ONLY
			GLint err = glGetError();
			assert(err==GL_NO_ERROR);
			m_data->m_instancePosOrnColor = new b3OpenCLArray<b3Vector4>(m_clData->m_clContext,m_clData->m_clQueue);
			m_data->m_instancePosOrnColor->resize(3*numObjects);
			m_data->m_instancePosOrnColor->copyFromHostPointer(positions,3*numObjects,0);
			glUnmapBuffer( GL_ARRAY_BUFFER);
			err = glGetError();
			assert(err==GL_NO_ERROR);
		}
	}
	
	{
		B3_PROFILE("stepSimulation");
		m_data->m_rigidBodyPipeline->stepSimulation(1./60.f);
		
	}

	if (numObjects)
	{
		B3_PROFILE("cl2gl_convert");
		int ciErrNum = 0;
		cl_mem bodies = m_data->m_rigidBodyPipeline->getBodyBuffer();
		b3LauncherCL launch(m_clData->m_clQueue,m_data->m_copyTransformsToVBOKernel);
		launch.setBuffer(bodies);
		launch.setBuffer(m_data->m_instancePosOrnColor->getBufferCL());
		launch.setConst(numObjects);
		launch.launch1D(numObjects);
		oclCHECKERROR(ciErrNum, CL_SUCCESS);
	}

	if (animate && numObjects)
	{
		B3_PROFILE("cl2gl_upload");
		GLint err = glGetError();
		assert(err==GL_NO_ERROR);
		GLuint vbo = m_instancingRenderer->getInternalData()->m_vbo;

		int arraySizeInBytes  = numObjects * (3)*sizeof(b3Vector4);

		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		cl_bool blocking=  CL_TRUE;
		positions=  (b3Vector4*)glMapBufferRange( GL_ARRAY_BUFFER,m_instancingRenderer->getMaxShapeCapacity(),arraySizeInBytes, GL_MAP_WRITE_BIT );//GL_READ_WRITE);//GL_WRITE_ONLY
		err = glGetError();
		assert(err==GL_NO_ERROR);
		m_data->m_instancePosOrnColor->copyToHostPointer(positions,3*numObjects,0);
		glUnmapBuffer( GL_ARRAY_BUFFER);
		err = glGetError();
		assert(err==GL_NO_ERROR);
	}

}

b3Vector3	GpuRigidBodyDemo::getRayTo(int x,int y)
{
	if (!m_instancingRenderer)
		return b3Vector3(0,0,0);
		
	float top = 1.f;
	float bottom = -1.f;
	float nearPlane = 1.f;
	float tanFov = (top-bottom)*0.5f / nearPlane;
	float fov = b3Scalar(2.0) * b3Atan(tanFov);

	b3Vector3 camPos,camTarget;
	m_instancingRenderer->getCameraPosition(camPos);
	m_instancingRenderer->getCameraTargetPosition(camTarget);

	b3Vector3	rayFrom = camPos;
	b3Vector3 rayForward = (camTarget-camPos);
	rayForward.normalize();
	float farPlane = 10000.f;
	rayForward*= farPlane;

	b3Vector3 rightOffset;
	b3Vector3 m_cameraUp(0,1,0);
	b3Vector3 vertical = m_cameraUp;

	b3Vector3 hor;
	hor = rayForward.cross(vertical);
	hor.normalize();
	vertical = hor.cross(rayForward);
	vertical.normalize();

	float tanfov = tanf(0.5f*fov);


	hor *= 2.f * farPlane * tanfov;
	vertical *= 2.f * farPlane * tanfov;

	b3Scalar aspect;
	float width = m_instancingRenderer->getScreenWidth();
	float height = m_instancingRenderer->getScreenHeight();

	aspect =  width / height;
	
	hor*=aspect;


	b3Vector3 rayToCenter = rayFrom + rayForward;
	b3Vector3 dHor = hor * 1.f/width;
	b3Vector3 dVert = vertical * 1.f/height;


	b3Vector3 rayTo = rayToCenter - 0.5f * hor + 0.5f * vertical;
	rayTo += b3Scalar(x) * dHor;
	rayTo -= b3Scalar(y) * dVert;
	return rayTo;
}

bool	GpuRigidBodyDemo::keyboardCallback(int key, int state)
{
	
	if (m_data)
	{
		if (key==B3G_ALT)
		{
			m_data->m_altPressed = state;
		}
		if (key==B3G_CONTROL)
		{
			m_data->m_controlPressed = state;
		}
	}
	return false;
}

bool	GpuRigidBodyDemo::mouseMoveCallback(float x,float y)
{
	if (m_data->m_altPressed!=0)
		return false;

	if (m_data->m_pickBody>=0 && m_data->m_pickConstraint>=0)
	{
		m_data->m_rigidBodyPipeline->removeConstraintByUid(m_data->m_pickConstraint);
		b3Vector3 newRayTo = getRayTo(x,y);
		b3Vector3 rayFrom;
		b3Vector3 oldPivotInB = m_data->m_pickPivotInB;
		b3Vector3 newPivotB;
		m_instancingRenderer->getCameraPosition(rayFrom);
		b3Vector3 dir = newRayTo-rayFrom;
		dir.normalize();
		dir *= m_data->m_pickDistance;
		newPivotB = rayFrom + dir;
		m_data->m_pickPivotInB = newPivotB;
		m_data->m_rigidBodyPipeline->copyConstraintsToHost();
		m_data->m_pickConstraint = m_data->m_rigidBodyPipeline->createPoint2PointConstraint(m_data->m_pickBody,m_data->m_pickFixedBody,m_data->m_pickPivotInA,m_data->m_pickPivotInB,1e30);
		m_data->m_rigidBodyPipeline->writeAllInstancesToGpu();
		return true;
	}
	return false;
}
bool	GpuRigidBodyDemo::mouseButtonCallback(int button, int state, float x, float y)
{

	if (state==1)
	{
		if(button==0 && (m_data->m_altPressed==0))
		{
			b3AlignedObjectArray<b3RayInfo> rays;
			b3AlignedObjectArray<b3RayHit> hitResults;
			b3Vector3 camPos;
			m_instancingRenderer->getCameraPosition(camPos);

			b3RayInfo ray;
			ray.m_from = camPos;
			ray.m_to = getRayTo(x,y);
			rays.push_back(ray);
			b3RayHit hit;
			hit.m_hitFraction = 1.f;
			hitResults.push_back(hit);
			m_data->m_rigidBodyPipeline->castRays(rays,hitResults);
			if (hitResults[0].m_hitFraction<1.f)
			{
				
				int hitBodyA = hitResults[0].m_hitBody;
				if (m_data->m_np->getBodiesCpu()[hitBodyA].m_invMass)
				{
					//printf("hit!\n");
					m_data->m_np->readbackAllBodiesToCpu();
					m_data->m_pickBody = hitBodyA;
					

				
			
					//pivotInA
					b3Vector3 pivotInB;
					pivotInB.setInterpolate3(ray.m_from,ray.m_to,hitResults[0].m_hitFraction);
					b3Vector3 posA;
					b3Quaternion ornA;
					m_data->m_np->getObjectTransformFromCpu(posA,ornA,hitBodyA);
					b3Transform tr;
					tr.setOrigin(posA);
					tr.setRotation(ornA);
					b3Vector3 pivotInA = tr.inverse()*pivotInB;
					if (m_data->m_pickFixedBody<0)
					{
						b3Vector3 pos(0,0,0);
						b3Quaternion orn(0,0,0,1);
						int fixedSphere = m_data->m_np->registerConvexHullShape(0,0,0,0);//>registerSphereShape(0.1);
						m_data->m_pickFixedBody = m_data->m_rigidBodyPipeline->registerPhysicsInstance(0,pos,orn,fixedSphere,0,false);
					
						if (m_data->m_pickGraphicsShapeIndex<0)
						{
							int strideInBytes = 9*sizeof(float);
							int numVertices = sizeof(point_sphere_vertices)/strideInBytes;
							int numIndices = sizeof(point_sphere_indices)/sizeof(int);
							m_data->m_pickGraphicsShapeIndex = m_instancingRenderer->registerShape(&point_sphere_vertices[0],numVertices,point_sphere_indices,numIndices,B3_GL_POINTS);
							float color[4] ={1,0,0,1};
							float scaling[4]={1,1,1,1};

							m_data->m_pickGraphicsShapeInstance = m_instancingRenderer->registerGraphicsInstance(m_data->m_pickGraphicsShapeIndex,pivotInB,orn,color,scaling);
							m_instancingRenderer->writeTransforms();
							delete m_data->m_instancePosOrnColor;
							m_data->m_instancePosOrnColor=0;
						} else
						{
							m_instancingRenderer->writeSingleInstanceTransformToCPU(pivotInB,orn,m_data->m_pickGraphicsShapeInstance);
							m_instancingRenderer->writeSingleInstanceTransformToGPU(pivotInB,orn,m_data->m_pickGraphicsShapeInstance);
							m_data->m_np->setObjectTransformCpu(pos,orn,m_data->m_pickFixedBody);
						}
			
					}
					pivotInB.w = 0.f;
					m_data->m_pickPivotInA = pivotInA;
					m_data->m_pickPivotInB = pivotInB;
					m_data->m_rigidBodyPipeline->copyConstraintsToHost();
					m_data->m_pickConstraint = m_data->m_rigidBodyPipeline->createPoint2PointConstraint(hitBodyA,m_data->m_pickFixedBody,pivotInA,pivotInB,1e30);//hitResults[0].m_hitResult0
					m_data->m_rigidBodyPipeline->writeAllInstancesToGpu();
					m_data->m_np->writeAllBodiesToGpu();
					m_data->m_pickDistance = (pivotInB-camPos).length();

					return true;
				}
			}
		}
	} else
	{
		if (button==0)
		{
			if (m_data->m_pickConstraint>=0)
			{
				m_data->m_rigidBodyPipeline->removeConstraintByUid(m_data->m_pickConstraint);
				m_data->m_pickConstraint=-1;
			}
		}
	}

	//printf("button=%d, state=%d\n",button,state);
	return false;
}