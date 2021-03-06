#include "ITMMotionAnalysis.h"
#include "../../../3rd_libLBFGS/lbfgs.h"
#include "../../Utils/KDtree/kdtree_search_eth.h"
#include "../../Utils/PointsIO/PointsIO.h"
#include <map>
#include "nlopt.hpp"

using namespace ITMLib::Engine;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// motions information

MotionsData::MotionsData(ITMMotionAnalysis *motionAnalysis, float *depth, int depth_width, int depth_height, const std::vector<Vector3f> &visiblePoints, const std::vector<Vector3f> &visibleNormals)
{
	alfa = 0.0f;
	beta = 0.0f;
	lambda = 0.0f;

	this->depth_image_width = depth_width;
	this->depth_image_height = depth_height;

	this->depth = depth;

	malys = motionAnalysis;
	this->visiblePoints = visiblePoints;
	this->visibleNormals = visibleNormals;

	ITMRGBDCalib *calib = NULL;
	std::vector<Transformation> tfs;
	std::vector<bool> visiblelist;
	malys->getAllPoints(points);
	malys->getAllNormals(normals);
	malys->getAllTransformations(tfs);
	malys->getCalib(calib);
	malys->getAllVisibleList(visiblelist);
	malys->getVisibleNeighbors(visible_neighbors);

	for (int i = 0; i < tfs.size(); i++){		
		x0.push_back(tfs[i].tx);
		x0.push_back(tfs[i].ty);
		x0.push_back(tfs[i].tz);
		x0.push_back(tfs[i].ry);
		x0.push_back(tfs[i].rz);
		x0.push_back(tfs[i].rx);
	}

	int count = 0;
	for (int i = 0; i < visiblelist.size(); i++){
		if (visiblelist[i]){
			visibles.push_back(count);
			count++;
		}
		else{
			visibles.push_back(-1);
		}
	}

	for (int i = 0; i < points.size(); i++){
		if (visibles[i] != -1){
			vpoints.push_back(Vector3f(points[i].x, points[i].y, points[i].z));
			vnormals.push_back(Vector3f(normals[i].x, normals[i].y, normals[i].z));
		}
	}

	//just for debug
	PointsIO::savePLYfile("vpoints.ply", vpoints, vnormals, Vector3u(255, 0, 0));

	computeDpoints(depth, x0, cdpoints, dpoints, 0.02);

	//just for debug
	std::vector<Vector3f> dnormals;
	PointsIO::savePLYfile("dpoints.ply", dpoints, dnormals, Vector3u(0, 0, 255));

    //get neighbors
	alfa = 1.0f;
	beta = 1.0f;

	Vector3f *pointSet = (Vector3f*)malloc(points.size()*sizeof(Vector3f));
	memset(pointSet, 0, points.size()*sizeof(Vector3f));

	for (int i = 0; i < points.size(); i++){
		pointSet[i].x = points[i].x;
		pointSet[i].y = points[i].y;
		pointSet[i].z = points[i].z;
	}

	KdTreeSearch_ETH kd_eth;
	kd_eth.add_vertex_set(pointSet, points.size());
	kd_eth.end();

	for (int i = 0; i < points.size(); i++){
		Vector3f p = points[i];
		std::vector<unsigned int> neighbors;
		std::vector<double> squared_distances;
		kd_eth.find_closest_K_points(p, 3, neighbors, squared_distances);

		neighborhood.push_back(neighbors);
	}

	kd_eth.begin();
	free(pointSet);
	pointSet = NULL;
}

void MotionsData::updateAllWarpInfo(const std::vector<double>& x)
{
	std::vector<Transformation> tfs;
	malys->getAllTransformations(tfs);

	for (int i = 0; i < tfs.size(); i++){
		tfs[i].tx = (float)x[6 * i];
		tfs[i].ty = (float)x[6 * i + 1];
		tfs[i].tz = (float)x[6 * i + 2];
		tfs[i].ry = (float)x[6 * i + 3];
		tfs[i].rz = (float)x[6 * i + 4];
		tfs[i].rx = (float)x[6 * i + 5];
	}
	malys->setAllTransformations(tfs);

	std::vector<Vector3f> trans_points;
	//just for debug
	for (int i = 0; i < points.size(); i++){
		std::vector<float> rot;
		std::vector<float> trans;
		malys->Transformation2RotTrans(tfs[i], rot, trans);
		Vector3f pt_tem = malys->TransformPoint(rot, trans, points[i]);

		trans_points.push_back(pt_tem);
	}

	std::vector<Vector3f> tnormals;
	PointsIO::savePLYfile("tpoints.ply", trans_points, tnormals, Vector3u(0, 255, 0));
}

void MotionsData::findNeighborsInDepthMap(int x, int y, int scale, std::vector<Vector2i> &pos_s){
	pos_s.clear();

	int span = scale / 2;

	if (span < 0){
		return;
	}

	for (int i = x - span; i < x + span; i++){
		for (int j = y - span; j < y + span; j++){
			if (i > 0 && i < depth_image_width-1 && j > 0 && j < depth_image_height-1){
				Vector2i position(i, j);
				pos_s.push_back(position);
			}
		}
	}
}

void MotionsData::computeDpoints(const float *depth, const std::vector<double>& x, std::vector<Vector3f> &cdps, std::vector<Vector3f> &dps, const float disThreshold)
{
	dps.clear();
	cdps.clear();

	ITMRGBDCalib *calib = NULL;
	malys->getCalib(calib);

	Vector4f projParams_d = calib->intrinsics_d.projectionParamsSimple.all;

	std::vector<Vector3f> vpts = visiblePoints;
	std::vector<Vector3f> vcpts = vpoints;

	if (malys->findDepthPointsPolicy == 0){
		int index = 0;
		for (int i = 0; i < points.size(); i++){
			if (visibles[i] != -1){
				Transformation tf = { x[6 * i], x[6 * i + 1], x[6 * i + 2], x[6 * i + 3], x[6 * i + 4], x[6 * i + 5] };
				std::vector<float> rot, trans;
				malys->Transformation2RotTrans(tf, rot, trans);

				std::vector<unsigned int> visible_nei = visible_neighbors[index];

				for (int k = 0; k < visible_nei.size(); k++){
					int id = visible_nei[k];
					vpts[id] = malys->TransformPoint(rot, trans, vpts[id]);
				}

				vcpts[index] = malys->TransformPoint(rot, trans, vcpts[index]);
				index++;
			}
		}

		std::vector<Vector3f> depthMap;
		for (int y = 0; y < depth_image_height; y++){
			for (int x = 0; x < depth_image_width; x++){
				Vector3f dpt;
				dpt.z = depth[x + y * depth_image_width];
				dpt.x = dpt.z * ((float(x) - projParams_d.z) / projParams_d.x);
				dpt.y = dpt.z * ((float(y) - projParams_d.w) / projParams_d.y);

				depthMap.push_back(dpt);
			}
		}

		for (int i = 0; i < vpts.size(); i++){
			Vector3f vpt = vpts[i];

			Vector2f pt_image;
			pt_image.x = projParams_d.x * vpt.x / vpt.z + projParams_d.z;
			pt_image.y = projParams_d.y * vpt.y / vpt.z + projParams_d.w;

			Vector3f dpt;

			if (!((pt_image.x < 1) || (pt_image.x > depth_image_width - 2) || (pt_image.y < 1) || (pt_image.y > depth_image_height - 2))){
				dpt = malys->interpolateBilinear(depthMap, Vector2f(pt_image.x, pt_image.y), Vector2i(depth_image_width, depth_image_height));

				/*dpt.z = depth[x + y * depth_image_width];
				dpt.x = dpt.z * ((float(x) - projParams_d.z) / projParams_d.x);
				dpt.y = dpt.z * ((float(y) - projParams_d.w) / projParams_d.y);*/
				if (dpt.z > 0){
					double dis_tem = (dpt.x - vpt.x)*(dpt.x - vpt.x) + (dpt.y - vpt.y)*(dpt.y - vpt.y) + (dpt.z - vpt.z)*(dpt.z - vpt.z);
					if (dis_tem > disThreshold*disThreshold){
						dpt.z = -1;
					}
				}
			}
			else{
				dpt.z = -1;
			}

			dps.push_back(dpt);
		}

		for (int i = 0; i < vcpts.size(); i++){
			Vector3f vpt = vcpts[i];

			Vector2f pt_image;
			pt_image.x = projParams_d.x * vpt.x / vpt.z + projParams_d.z;
			pt_image.y = projParams_d.y * vpt.y / vpt.z + projParams_d.w;

			Vector3f dpt;

			if (!((pt_image.x < 1) || (pt_image.x > depth_image_width - 2) || (pt_image.y < 1) || (pt_image.y > depth_image_height - 2))){
				dpt = malys->interpolateBilinear(depthMap, Vector2f(pt_image.x, pt_image.y), Vector2i(depth_image_width, depth_image_height));

				/*dpt.z = depth[x + y * depth_image_width];
				dpt.x = dpt.z * ((float(x) - projParams_d.z) / projParams_d.x);
				dpt.y = dpt.z * ((float(y) - projParams_d.w) / projParams_d.y);*/
				if (dpt.z > 0){
					double dis_tem = (dpt.x - vpt.x)*(dpt.x - vpt.x) + (dpt.y - vpt.y)*(dpt.y - vpt.y) + (dpt.z - vpt.z)*(dpt.z - vpt.z);
					if (dis_tem > disThreshold*disThreshold){
						dpt.z = -1;
					}
				}
			}
			else{
				dpt.z = -1;
			}

			cdps.push_back(dpt);
		}

		//just for debug
		std::vector<Vector3f> dnormals;

		std::vector<Vector3u> visibleColors;
		visibleColors.resize(visiblePoints.size());
		for (int i = 0; i < visiblePoints.size(); i++){
			visibleColors[i] = Vector3u(255, 255, 255);
		}

		index = 0;
		for (int i = 0; i < points.size(); i++){
			if (visibles[i] != -1){
				std::vector<unsigned int> visible_nei = visible_neighbors[index];

				Vector3u color;
				color[0] = rand() % 256;
				color[1] = rand() % 256;
				color[2] = rand() % 256;

				for (int k = 0; k < visible_nei.size(); k++){
					int id = visible_nei[k];
					visibleColors[id] = color;
				}
				index++;
			}
		}
		PointsIO::savePLYfile("vpts.ply", vpts, dnormals, visibleColors);
		PointsIO::savePLYfile("dps.ply", dps, dnormals, visibleColors);
		PointsIO::savePLYfile("depthMap.ply", depthMap, dnormals, Vector3u(255, 255, 0));
		PointsIO::savePLYfile("cdps.ply", cdps, dnormals, Vector3u(255, 0, 0));

	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// objective function
double motions_function(const std::vector<double> &x, std::vector<double> &grad, void *data)
{
	MotionsData* d = (MotionsData*)data;
	std::vector<Vector3f> points = d->points;
	//const std::vector<Vector3f>& normals = d->normals;
	const std::vector<Vector3f>& dpoints = d->dpoints;
	const std::vector<Vector3f>& cdpoints = d->cdpoints;
	const std::vector<int>& visibles = d->visibles;
	const std::vector<double>& x0 = d->x0;
	const bool rotationOnly = d->rotationOnly;
	const std::vector<std::vector<unsigned int>>& neighborhood = d->neighborhood;
	ITMMotionAnalysis* malys = d->malys;
	float lambda = d->lambda;
	float alfa = d->alfa;
	float beta = d->beta;

	std::vector<Vector3f> visiblePoints = d->visiblePoints;
	std::vector<Vector3f> visibleNormals = d->visibleNormals;
	std::vector<std::vector<unsigned int>> &visible_neighbors = d->visible_neighbors;

	for (int i = 0; i < (int)x.size() / 6; i++) {
		Transformation gtf = { x0[6 * i], x0[6 * i + 1], x0[6 * i + 2], x0[6 * i + 3], x0[6 * i + 4], x0[6 * i + 5] };
		std::vector<float> grot, gtrans;
		malys->Transformation2RotTrans(gtf, grot, gtrans);
		points[i] = malys->TransformPoint(grot, gtrans, points[i]);
	}

	//////////////////////////////////////////////////////////////////////////
	// initialize
	double f = 0;
	for (int i = 0; i < (int)x.size(); ++i) {
		grad[i] = 0;
	}

	std::vector<Vector3f> npts;
	// data term
	for (int i = 0; i < (int)x.size() / 6; i++) {
		if (visibles[i] == -1)
			continue;

		std::vector<unsigned int> visible_nei = visible_neighbors[visibles[i]];

		// warp transformation of i
		Transformation tf = { x[6 * i], x[6 * i + 1], x[6 * i + 2], x[6 * i + 3], x[6 * i + 4], x[6 * i + 5] };
		std::vector<float> rot, trans;
		malys->Transformation2RotTrans(tf, rot, trans);

		Transformation gtf = { x0[6 * i], x0[6 * i + 1], x0[6 * i + 2], x0[6 * i + 3], x0[6 * i + 4], x0[6 * i + 5] };
		std::vector<float> grot, gtrans;
		malys->Transformation2RotTrans(gtf, grot, gtrans);

		for (int k = 0; k < visible_nei.size(); k++){
			int id = visible_nei[k];

			visiblePoints[id] = malys->TransformPoint(grot, gtrans, visiblePoints[id]);
			visibleNormals[id] = malys->TransformNormal(grot, visibleNormals[id]);

			// sum of data term
			Vector3f vi = visiblePoints[id];
			Vector3f ni = visibleNormals[id];
			Vector3f mvi = malys->TransformPoint(rot, trans, visiblePoints[id]);
			Vector3f mni = malys->TransformNormal(rot, visibleNormals[id]);
			Vector3f dvi = dpoints[id];

			if (dvi.z == -1){
				continue;
			}

			Vector3f delta_mvi_dvi(mvi.x - dvi.x, mvi.y - dvi.y, mvi.z - dvi.z);
			double dot1 = mni.x * delta_mvi_dvi.x + mni.y * delta_mvi_dvi.y + mni.z * delta_mvi_dvi.z;

			f += dot1 * dot1;

			/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
			// gradient 
			// 2 * (ni.(vi - vi'))
			double dot2 = dot1;
			dot2 *= 2.0f;

			if (!rotationOnly){
				// tx, ty, tz gradient
				grad[i * 6] += dot2 * mni.x;
				grad[i * 6 + 1] += dot2 * mni.y;
				grad[i * 6 + 2] += dot2 * mni.z;
			}

			float chi = std::cos(x[i * 6 + 3]);
			float shi = std::sin(x[i * 6 + 3]);
			float cai = std::cos(x[i * 6 + 4]);
			float sai = std::sin(x[i * 6 + 4]);
			float cbi = std::cos(x[i * 6 + 5]);
			float sbi = std::sin(x[i * 6 + 5]);

			float chi_ = -std::sin(x[i * 6 + 3]);
			float shi_ = std::cos(x[i * 6 + 3]);
			float cai_ = -std::sin(x[i * 6 + 4]);
			float sai_ = std::cos(x[i * 6 + 4]);
			float cbi_ = -std::sin(x[i * 6 + 5]);
			float sbi_ = std::cos(x[i * 6 + 5]);

			// ry gradient
			// (mtf * ni) . (mtf * vi) - (mtf * ni) . dvi gradient
			grad[i * 6 + 3] += (mvi.x - dvi.x) * (chi_ * cai * ni.x + (shi_ * sbi - chi_ * sai * cbi) * ni.y + (chi_ * sai * sbi + shi_ * cbi) * ni.z);
			grad[i * 6 + 3] += mni.x * (chi_ * cai * vi.x + (shi_ * sbi - chi_ * sai * cbi) * vi.y + (chi_ * sai * sbi + shi_ * cbi) * vi.z);
			grad[i * 6 + 3] += (mvi.z - dvi.z) * (-shi_ * cai * ni.x + (shi_ * sai * cbi + chi_ * sbi) * ni.y + (-shi_ * sai * sbi + chi_ * cbi) * ni.z);
			grad[i * 6 + 3] += mni.z * (-shi_ * cai * vi.x + (shi_ * sai * cbi + chi_ * sbi) * vi.y + (-shi_ * sai * sbi + chi_ * cbi) * vi.z);
			// 2 * (ni.(vi - vi')) * ((mtf * ni) . (mtf * vi) - (mtf * ni) . dvi)'ry
			grad[i * 6 + 3] *= dot2;

			// rz gradient
			// (mtf * ni) . (mtf * vi) - (mtf * ni) . dvi gradient
			grad[i * 6 + 4] += (mvi.x - dvi.x) * (chi * cai_ * ni.x + (-chi * sai_ * cbi) * ni.y + (chi * sai_ * sbi) * ni.z);
			grad[i * 6 + 4] += mni.x * (chi * cai_ * vi.x + (-chi * sai_ * cbi) * vi.y + (chi * sai_ * sbi) * vi.z);
			grad[i * 6 + 4] += (mvi.y - dvi.y) * (sai_ * ni.x + cai_ * cbi * ni.y + (-cai_ * sbi) * ni.z);
			grad[i * 6 + 4] += mni.y * (sai_ * vi.x + cai_ * cbi * vi.y + (-cai_ * sbi) * vi.z);
			grad[i * 6 + 4] += (mvi.z - dvi.z) * ((-shi * cai_)* ni.x + shi * sai_ * cbi * ni.y + (-shi * sai_ * sbi) * ni.z);
			grad[i * 6 + 4] += mni.z * ((-shi * cai_)* vi.x + shi * sai_ * cbi * vi.y + (-shi * sai_ * sbi) * vi.z);
			// 2 * (ni.(vi - vi')) * ((mtf * ni) . (mtf * vi) - (mtf * ni) . dvi)'rz
			grad[i * 6 + 4] *= dot2;

			// rx gradient
			// (mtf * ni) . (mtf * vi) - (mtf * ni) . dvi gradient
			grad[i * 6 + 5] += (mvi.x - dvi.x) * ((shi * sbi_ - chi * sai * cbi_)* ni.y + (chi * sai * sbi_ + shi * cbi_) * ni.z);
			grad[i * 6 + 5] += mni.x * ((shi * sbi_ - chi * sai * cbi_)* vi.y + (chi * sai * sbi_ + shi * cbi_) * vi.z);
			grad[i * 6 + 5] += (mvi.y - dvi.y) * (cai * cbi_ * ni.y + (-cai * sbi_) * ni.z);
			grad[i * 6 + 5] += mni.y * (cai * cbi_ * vi.y + (-cai * sbi_) * vi.z);
			grad[i * 6 + 5] += (mvi.z - dvi.z) * ((shi * sai * cbi_ + chi * cbi_) * ni.y + (-shi * sai * sbi_ + chi * cbi_) * ni.z);
			grad[i * 6 + 5] += mni.z * ((shi * sai * cbi_ + chi * cbi_) * vi.y + (-shi * sai * sbi_ + chi * cbi_) * vi.z);
			// 2 * (ni.(vi - vi')) * ((mtf * ni) . (mtf * vi) - (mtf * ni) . dvi)'rx
			grad[i * 6 + 5] *= dot2;
		}
	}

	//reg term1
	for (int i = 0; i < (int)x.size() / 6; i++){
		const std::vector<unsigned int>& neighbors = neighborhood[i];
		Transformation tf_ic = { x[6 * i], x[6 * i + 1], x[6 * i + 2], x[6 * i + 3], x[6 * i + 4], x[6 * i + 5] };
		std::vector<float> iRot, iTrans;
		malys->Transformation2RotTrans(tf_ic, iRot, iTrans);

		float factor = alfa;

		//compute each sub term
		for (int j = 0; j < neighbors.size(); j++){
			unsigned int ind = neighbors[j];
			if (ind == i || points[i] == points[ind]){
				continue;
			}

			if (visibles[i] == -1 || visibles[ind] == -1){
				continue;
			}

			Vector3f vi = points[i];
			Vector3f vj = points[ind];
			Vector3f di = cdpoints[visibles[i]];
			Vector3f dj = cdpoints[visibles[ind]];

			if (di.z == -1 || dj.z == -1){
				continue;
			}

			// sum of reg term
			Vector3f vij = vi - vj;
			Vector3f dij = di - dj;

			Vector3f mvij = malys->TransformNormal(iRot, vij);

			Vector3f delta = mvij - dij;
			double result_tem = (delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
			f += factor * result_tem;

			/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
			// gradient 
			float chi = std::cos(x[i * 6 + 3]);
			float shi = std::sin(x[i * 6 + 3]);
			float cai = std::cos(x[i * 6 + 4]);
			float sai = std::sin(x[i * 6 + 4]);
			float cbi = std::cos(x[i * 6 + 5]);
			float sbi = std::sin(x[i * 6 + 5]);

			float chi_ = -std::sin(x[i * 6 + 3]);
			float shi_ = std::cos(x[i * 6 + 3]);
			float cai_ = -std::sin(x[i * 6 + 4]);
			float sai_ = std::cos(x[i * 6 + 4]);
			float cbi_ = -std::sin(x[i * 6 + 5]);
			float sbi_ = std::cos(x[i * 6 + 5]);

			// i, ry, rz, rx gradient
			// ry
			grad[i * 6 + 3] += factor * 2.0f * delta.x * (chi_ * cai * vij.x + (shi_ * sbi - chi_ * sai * cbi) * vij.y + (chi_ * sai * sbi + shi_ * cbi) * vij.z);
			grad[i * 6 + 3] += factor * 2.0f * delta.z * ((-shi_ * cai) * vij.x + (shi_ * sai * cbi + chi_ * sbi) * vij.y + (-shi_ * sai * sbi + chi_ * cbi) * vij.z);

			// rz
			grad[i * 6 + 4] += factor * 2.0f * delta.x * ((chi * cai_) * vij.x + (-chi * sai_ * cbi) * vij.y + (chi * sai_ * sbi) * vij.z);
			grad[i * 6 + 4] += factor * 2.0f * delta.y * (sai_* vij.x + (cai_ * cbi) * vij.y + (-cai_ * sbi) * vij.z);
			grad[i * 6 + 4] += factor * 2.0f * delta.z * ((-shi * cai_)* vij.x + (shi * sai_ * cbi) * vij.y + (-shi * sai_ * sbi) * vij.z);

			// rx
			grad[i * 6 + 5] += factor * 2.0f * delta.x * ((shi * sbi_ - chi * sai * cbi_) * vij.y + (chi * sai * sbi_ + shi * cbi_) * vij.z);
			grad[i * 6 + 5] += factor * 2.0f * delta.y * ((cai * cbi_)* vij.y + (-cai * sbi_) * vij.z);
			grad[i * 6 + 5] += factor * 2.0f * delta.z * ((shi * sai * cbi_ + chi * sbi_) * vij.y + (-shi * sai * sbi_ + chi * cbi_) * vij.z);
		}
	}

	//reg term2
	for (int i = 0; i < (int)x.size() / 6; i++){
		const std::vector<unsigned int>& neighbors = neighborhood[i];
		Transformation tf_ic = { x[6 * i], x[6 * i + 1], x[6 * i + 2], x[6 * i + 3], x[6 * i + 4], x[6 * i + 5] };
		std::vector<float> iRot, iTrans;
		malys->Transformation2RotTrans(tf_ic, iRot, iTrans);

		//float factor = lambda;
		float factor = beta;

		//compute each sub term
		for (int j = 0; j < neighbors.size(); j++){
			unsigned int ind = neighbors[j];
			if (ind == i || points[i] == points[ind]){
				continue;
			}

			Transformation tf_jc = { x[6 * ind], x[6 * ind + 1], x[6 * ind + 2], x[6 * ind + 3], x[6 * ind + 4], x[6 * ind + 5] };
			std::vector<float> jRot, jTrans;
			malys->Transformation2RotTrans(tf_jc, jRot, jTrans);

			Vector3f vi = points[i];
			Vector3f vj = points[ind];
			Vector3f mvij = malys->TransformPoint(iRot, iTrans, vj);
			Vector3f mvjj = malys->TransformPoint(jRot, jTrans, vj);

			// sum of reg term
			Vector3f vij = vi - vj;
			//float squared_distance = vij.x * vij.x + vij.y * vij.y + vij.z * vij.z;
			Vector3f delta = mvij - mvjj;
			//double result_tem = (delta.x * delta.x + delta.y * delta.y + delta.z * delta.z) / squared_distance;
			double result_tem = (delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
			f += factor * result_tem;

			/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
			// gradient 
			float chi = std::cos(x[i * 6 + 3]);
			float shi = std::sin(x[i * 6 + 3]);
			float cai = std::cos(x[i * 6 + 4]);
			float sai = std::sin(x[i * 6 + 4]);
			float cbi = std::cos(x[i * 6 + 5]);
			float sbi = std::sin(x[i * 6 + 5]);

			float chi_ = -std::sin(x[i * 6 + 3]);
			float shi_ = std::cos(x[i * 6 + 3]);
			float cai_ = -std::sin(x[i * 6 + 4]);
			float sai_ = std::cos(x[i * 6 + 4]);
			float cbi_ = -std::sin(x[i * 6 + 5]);
			float sbi_ = std::cos(x[i * 6 + 5]);

			if (!rotationOnly){
				// i, tx, ty, tz
				grad[i * 6] += factor * 2.0f * (mvij.x - mvjj.x) * 1.0f;
				grad[i * 6 + 1] += factor * 2.0f * (mvij.y - mvjj.y) * 1.0f;
				grad[i * 6 + 2] += factor * 2.0f * (mvij.z - mvjj.z) * 1.0f;
			}

			// i, ry, rz, rx gradient
			// ry
			grad[i * 6 + 3] += (factor)* 2.0f * (mvij.x - mvjj.x) * (chi_ * cai * vj.x + (shi_ * sbi - chi_ * sai * cbi) * vj.y + (chi_ * sai * sbi + shi_ * cbi) * vj.z);
			grad[i * 6 + 3] += (factor)* 2.0f * (mvij.z - mvjj.z) * ((-shi_ * cai) * vj.x + (shi_ * sai * cbi + chi_ * sbi) * vj.y + (-shi_ * sai * sbi + chi_ * cbi) * vj.z);

			// rz
			grad[i * 6 + 4] += (factor)* 2.0f * (mvij.x - mvjj.x) * ((chi * cai_) * vj.x + (-chi * sai_ * cbi) * vj.y + (chi * sai_ * sbi) * vj.z);
			grad[i * 6 + 4] += (factor)* 2.0f * (mvij.y - mvjj.y) * (sai_* vj.x + (cai_ * cbi) * vj.y + (-cai_ * sbi) * vj.z);
			grad[i * 6 + 4] += (factor)* 2.0f * (mvij.z - mvjj.z) * ((-shi * cai_)* vj.x + (shi * sai_ * cbi) * vj.y + (-shi * sai_ * sbi) * vj.z);

			// rx
			grad[i * 6 + 5] += (factor)* 2.0f * (mvij.x - mvjj.x) * ((shi * sbi_ - chi * sai * cbi_) * vj.y + (chi * sai * sbi_ + shi * cbi_) * vj.z);
			grad[i * 6 + 5] += (factor)* 2.0f * (mvij.y - mvjj.y) * ((cai * cbi_)* vj.y + (-cai * sbi_) * vj.z);
			grad[i * 6 + 5] += (factor)* 2.0f * (mvij.z - mvjj.z) * ((shi * sai * cbi_ + chi * sbi_) * vj.y + (-shi * sai * sbi_ + chi * cbi_) * vj.z);

			if (!rotationOnly){
				// j, tx, ty, tz
				grad[ind * 6] += factor * 2.0f * (mvij.x - mvjj.x) * (-1.0f);
				grad[ind * 6 + 1] += factor * 2.0f * (mvij.y - mvjj.y) * (-1.0f);
				grad[ind * 6 + 2] += factor * 2.0f * (mvij.z - mvjj.z) * (-1.0f);
			}

			// j, ry, rz, rx gradient
			// ry
			grad[ind * 6 + 3] += (-factor) * 2.0f * (mvij.x - mvjj.x) * (chi_ * cai * vj.x + (shi_ * sbi - chi_ * sai * cbi) * vj.y + (chi_ * sai * sbi + shi_ * cbi) * vj.z);
			grad[ind * 6 + 3] += (-factor) * 2.0f * (mvij.z - mvjj.z) * ((-shi_ * cai) * vj.x + (shi_ * sai * cbi + chi_ * sbi) * vj.y + (-shi_ * sai * sbi + chi_ * cbi) * vj.z);

			// rz
			grad[ind * 6 + 4] += (-factor) * 2.0f * (mvij.x - mvjj.x) * ((chi * cai_) * vj.x + (-chi * sai_ * cbi) * vj.y + (chi * sai_ * sbi) * vj.z);
			grad[ind * 6 + 4] += (-factor) * 2.0f * (mvij.y - mvjj.y) * (sai_* vj.x + (cai_ * cbi) * vj.y + (-cai_ * sbi) * vj.z);
			grad[ind * 6 + 4] += (-factor) * 2.0f * (mvij.z - mvjj.z) * ((-shi * cai_)* vj.x + (shi * sai_ * cbi) * vj.y + (-shi * sai_ * sbi) * vj.z);

			// rx
			grad[ind * 6 + 5] += (-factor) * 2.0f * (mvij.x - mvjj.x) * ((shi * sbi_ - chi * sai * cbi_) * vj.y + (chi * sai * sbi_ + shi * cbi_) * vj.z);
			grad[ind * 6 + 5] += (-factor) * 2.0f * (mvij.y - mvjj.y) * ((cai * cbi_)* vj.y + (-cai * sbi_) * vj.z);
			grad[ind * 6 + 5] += (-factor) * 2.0f * (mvij.z - mvjj.z) * ((shi * sai * cbi_ + chi * sbi_) * vj.y + (-shi * sai * sbi_ + chi * cbi_) * vj.z);
		}
	}

	return f;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// main optimization function
void ITMMotionAnalysis::optimizeEnergyFunctionNlopt(ITMFloatImage *newDepthImage, const std::vector<Vector3f> &visiblePoints, const std::vector<Vector3f> &visibleNormals)
{
	int depth_image_width = newDepthImage->noDims.x;
	int depth_image_height = newDepthImage->noDims.y;
	float *depth_device = newDepthImage->GetData(MEMORYDEVICE_CUDA);
	float *depth = (float*)malloc(depth_image_width*depth_image_height * sizeof(float));
	ITMSafeCall(cudaMemcpy(depth, depth_device, (depth_image_width * depth_image_height)*sizeof(float), cudaMemcpyDeviceToHost));

	MotionsData data(this, depth, depth_image_width, depth_image_height, visiblePoints, visibleNormals);

	float old_f = 1e10;
	float disThreshold = 0.1;
	for (int k = 0; k < 6; k++){
		//optimization
		int n = data.x0.size();
		std::vector<double> x(n);
		for (int i = 0; i < n; ++i) {
			x[i] = 0;
		}

		data.computeDpoints(depth, data.x0, data.cdpoints, data.dpoints, disThreshold);

		if (k < 3){
			data.rotationOnly = true;
		}
		else{
			data.rotationOnly = false;
		}

		disThreshold -= (disThreshold / 6);

		nlopt::opt opt(nlopt::LD_MMA, n);
		opt.set_min_objective(motions_function, &data);
		opt.set_xtol_rel(1e-5);
		opt.set_maxeval(1000);

		double minf;
		nlopt::result result;
		result = opt.optimize(x, minf);
		std::cout << "minf = " << minf << std::endl;
		std::cout << "result = " << result << std::endl;

		/////////////////////////////////

		//update
		if (minf < old_f && result == 4){
			old_f = minf;

			for (int i = 0; i < n / 6; ++i) {
				Transformation step = { x[6 * i], x[6 * i + 1], x[6 * i + 2], x[6 * i + 3], x[6 * i + 4], x[6 * i + 5] };
				Transformation oldTransformation = { data.x0[6 * i], data.x0[6 * i + 1], data.x0[6 * i + 2], data.x0[6 * i + 3], data.x0[6 * i + 4], data.x0[6 * i + 5] };
				Transformation newTransformation = { 0, 0, 0, 0, 0, 0 };
				updateTransformation(step, oldTransformation, newTransformation);

				data.x0[6 * i] = newTransformation.tx;
				data.x0[6 * i + 1] = newTransformation.ty;
				data.x0[6 * i + 2] = newTransformation.tz;
				data.x0[6 * i + 3] = newTransformation.ry;
				data.x0[6 * i + 4] = newTransformation.rz;
				data.x0[6 * i + 5] = newTransformation.rx;
			}
		}
		else{
			std::cout << "minf>=old_f or result != 1" << std::endl;
		}
	}

	data.updateAllWarpInfo(data.x0);

	free(depth);
	depth = NULL;
}