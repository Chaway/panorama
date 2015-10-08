// File: stitcher.cc
// Date: Sun Sep 22 12:54:18 2013 +0800
// Author: Yuxin Wu <ppwwyyxxc@gmail.com>


#include "stitcher.hh"

#include <fstream>
#include <algorithm>

#include "feature/matcher.hh"
#include "warp.hh"
#include "transform_estimate.hh"
#include "projection.hh"
#include "match_info.hh"

#include "lib/timer.hh"
#include "lib/imgproc.hh"
#include "blender.hh"
using namespace std;
using namespace feature;

Mat32f Stitcher::build() {
	calc_feature();
	if (PANO) {
		build_bundle_warp();
		bundle.proj_method = ConnectedImages::ProjectionMethod::flat;
	} else {
	  //pairwise_match();
		assume_pano_pairwise();
		estimate_camera();
		build_bundle_linear_simple();
		bundle.proj_method = ConnectedImages::ProjectionMethod::cylindrical;
	}
	print_debug("Using projection method: %d\n", bundle.proj_method);
	bundle.update_proj_range();

	return blend();
}

void Stitcher::calc_feature() {
	GuardedTimer tm("calc_feature()");
	int n = imgs.size();
	// detect feature
#pragma omp parallel for schedule(dynamic)
	REP(k, n) {
		feats[k] = feature_det->detect_feature(imgs[k]);
		print_debug("Image %d has %lu features\n", k, feats[k].size());
	}
}

void Stitcher::pairwise_match() {
	GuardedTimer tm("pairwise_match()");
	size_t n = imgs.size();

	REP(i, n) REPL(j, i + 1, n) {
		FeatureMatcher matcher(feats[i], feats[j]);
		auto match = matcher.match();
		TransformEstimation transf(match, feats[i], feats[j]);
		MatchInfo info;
		bool succ = transf.get_transform(&info);
		if (succ) {
			print_debug(
					"Connection between image %lu and %lu, ninliers=%lu, conf=%f\n",
					i, j, info.match.size(), info.confidence);
			graph[i].push_back(j);
			graph[j].push_back(i);
			pairwise_matches[i][j] = info;
			info.homo = info.homo.inverse();
			pairwise_matches[j][i] = move(info);
		}
	}
}

void Stitcher::assume_pano_pairwise() {
	GuardedTimer tm("assume_pano_pairwise()");
	int n = imgs.size();
	REP(i, n) {
		int next = (i + 1) % n;
		FeatureMatcher matcher(feats[i], feats[next]);
		auto match = matcher.match();
		TransformEstimation transf(match, feats[i], feats[next]);
		MatchInfo info;
		bool succ = transf.get_transform(&info);
		if (not succ)
			error_exit(ssprintf("Image %d and %d doesn't match.\n", i, next));
		print_debug("Match between image %d and %d, ninliers=%lu, conf=%f\n",
				i, next, info.match.size(), info.confidence);
		graph[i].push_back(next);
		graph[next].push_back(i);
		pairwise_matches[i][next] = info;
		info.homo = info.homo.inverse();
		pairwise_matches[next][i] = move(info);
	}
}

void Stitcher::estimate_camera() {
	int n = imgs.size();
	{ // assign an initial focal length
		double focal = Camera::estimate_focal(pairwise_matches);
		if (focal > 0)
			for (auto& c : cameras)
				c.focal = focal;
		else
			REP(i, n) // hack focal
				cameras[i].focal = (imgs[i].width() / imgs[i].height()) * 0.5;
	}
	// TODO estimate camera rotation

}

Mat32f Stitcher::blend() {
	GuardedTimer tm("blend()");
	int refw = imgs[bundle.identity_idx].width(),
			refh = imgs[bundle.identity_idx].height();
	auto homo2proj = bundle.get_homo2proj();
	auto proj2homo = bundle.get_proj2homo();

	Vec2D id_img_range = homo2proj(Vec(1, 1, 1)) - homo2proj(Vec(0, 0, 1));
	id_img_range.x *= refw, id_img_range.y *= refh;
	cout << "id_img_range" << id_img_range << endl;
	cout << "projmin:" << bundle.proj_range.min << "projmax" << bundle.proj_range.max << endl;

	Vec2D proj_min = bundle.proj_range.min;
	real_t x_len = bundle.proj_range.max.x - proj_min.x,
				 y_len = bundle.proj_range.max.y - proj_min.y,
				 x_per_pixel = id_img_range.x / refw,
				 y_per_pixel = id_img_range.y / refh,
				 target_width = x_len / x_per_pixel,
				 target_height = y_len / y_per_pixel;

	Coor size(target_width, target_height);
	cout << "Final Image Size: " << size << endl;

	auto scale_coor_to_img_coor = [&](Vec2D v) {
		v = v - proj_min;
		v.x /= x_per_pixel, v.y /= y_per_pixel;
		return Coor(v.x, v.y);
	};

	// blending
	Mat32f ret(size.y, size.x, 3);
	fill(ret, Color::NO);

	LinearBlender blender;
	for (auto& cur : bundle.component) {
		Coor top_left = scale_coor_to_img_coor(cur.range.min);
		Coor bottom_right = scale_coor_to_img_coor(cur.range.max);
		Coor diff = bottom_right - top_left;
		int w = diff.x, h = diff.y;
		Mat<Vec2D> orig_pos(h, w, 1);

		REP(i, h) REP(j, w) {
			Vec2D c((j + top_left.x) * x_per_pixel + proj_min.x, (i + top_left.y) * y_per_pixel + proj_min.y);
			Vec homo = proj2homo(Vec2D(c.x / refw, c.y / refh));
			homo.x -= 0.5 * homo.z, homo.y -= 0.5 * homo.z;	// shift center for homography
			homo.x *= refw, homo.y *= refh;
			Vec2D& p = (orig_pos.at(i, j)
					= cur.homo_inv.trans_normalize(homo)
					+ Vec2D(cur.imgptr->width()/2, cur.imgptr->height()/2));
			if (!p.isNaN() && (p.x < 0 || p.x >= cur.imgptr->width()
						|| p.y < 0 || p.y >= cur.imgptr->height()))
				p = Vec2D::NaN();
		}
		blender.add_image(top_left, orig_pos, *cur.imgptr);
	}
	blender.run(ret);
	if (PANO)
		return perspective_correction(ret);
	return ret;
}

void Stitcher::straighten_simple() {
	int n = imgs.size();
	Vec2D center2 = bundle.component[n - 1].homo.trans2d(0, 0);
	Vec2D center1 = bundle.component[0].homo.trans2d(0, 0);
	float dydx = (center2.y - center1.y) / (center2.x - center1.x);
	Matrix S = Matrix::I(3);
	S.at(1, 0) = dydx;
	Matrix Sinv(3, 3);
	bool succ = S.inverse(Sinv);
	m_assert(succ);
	REP(i, n) bundle.component[i].homo = Sinv.prod(bundle.component[i].homo);
}


void Stitcher::build_bundle_linear_simple() {
	// assume pano pairwise
	int n = imgs.size(), mid = n >> 1;
	bundle.identity_idx = mid;
	bundle.component[mid].homo = Homography::I();

	auto& comp = bundle.component;

	// accumulate the transformations
	comp[mid+1].homo = pairwise_matches[mid][mid+1].homo;
	REPL(k, mid + 2, n)
		comp[k].homo = Homography(
				comp[k - 1].homo.prod(pairwise_matches[k][k-1].homo));
	comp[mid-1].homo = pairwise_matches[mid][mid-1].homo;
	REPD(k, mid - 2, 0)
		comp[k].homo = Homography(
				comp[k + 1].homo.prod(pairwise_matches[k][k+1].homo));
	// then, comp[k]: from k to identity
	bundle.calc_inverse_homo();
}


void Stitcher::build_bundle_warp() {;
	GuardedTimer tm("build_bundle_warp()");
	int n = imgs.size(), mid = n >> 1;
	bundle.identity_idx = mid;
	REP(i, n) bundle.component[i].homo = Homography::I();

	Timer timer;
	vector<MatchData> matches;		// matches[k]: k,k+1
	matches.resize(n-1);
	REP(k, n - 1) {
		FeatureMatcher matcher(feats[k], feats[(k + 1) % n]);
		matches[k] = matcher.match();
	}
	print_debug("match time: %lf secs\n", timer.duration());

	vector<Homography> bestmat;

	float minslope = numeric_limits<float>::max();
	float bestfactor = 1;
	if (n - mid > 1) {
		float newfactor = 1;
		// XXX: ugly
		float slope = update_h_factor(newfactor, minslope, bestfactor, bestmat, matches);
		if (bestmat.empty())
			error_exit("Failed to find hfactor");
		float centerx1 = 0, centerx2 = bestmat[0].trans2d(0, 0).x;
		float order = (centerx2 > centerx1 ? 1 : -1);
		REP(k, 3) {
			if (fabs(slope) < SLOPE_PLAIN) break;
			newfactor += (slope < 0 ? order : -order) / (5 * pow(2, k));
			slope = Stitcher::update_h_factor(newfactor, minslope, bestfactor, bestmat, matches);
		}
	}
	print_debug("Best hfactor: %lf\n", bestfactor);
	CylinderWarper warper(bestfactor);
#pragma omp parallel for schedule(dynamic)
	REP(k, n) warper.warp(imgs[k], feats[k]);

	// accumulate
	REPL(k, mid + 1, n) bundle.component[k].homo = move(bestmat[k - mid - 1]);
#pragma omp parallel for schedule(dynamic)
	REPD(i, mid - 1, 0) {
		matches[i].reverse();
		MatchInfo info;
		bool succ = TransformEstimation(
				matches[i], feats[i + 1], feats[i]).get_transform(&info);
		if (not succ)
			error_exit(ssprintf("Image %d and %d doesn't match. Failed", i, i+1));
		bundle.component[i].homo = info.homo;
	}
	REPD(i, mid - 2, 0)
		bundle.component[i].homo = Homography(
				bundle.component[i + 1].homo.prod(bundle.component[i].homo));
	bundle.calc_inverse_homo();
}

// XXX ugly hack
float Stitcher::update_h_factor(float nowfactor,
		float & minslope,
		float & bestfactor,
		vector<Homography>& mat,
		const vector<MatchData>& matches) {
	const int n = imgs.size(), mid = bundle.identity_idx;
	const int start = mid, end = n, len = end - start;

	vector<Mat32f> nowimgs;
	vector<vector<Descriptor>> nowfeats;
	REPL(k, start, end) {
		nowimgs.push_back(imgs[k].clone());
		nowfeats.push_back(feats[k]);
	}			// nowfeats[0] == feats[mid]

	CylinderWarper warper(nowfactor);
#pragma omp parallel for schedule(dynamic)
	REP(k, len)
		warper.warp(nowimgs[k], nowfeats[k]);

	vector<Homography> nowmat;		// size = len - 1
	nowmat.resize(len - 1);
	bool failed = false;
#pragma omp parallel for schedule(dynamic)
	REPL(k, 1, len) {
		MatchInfo info;
		bool succ = TransformEstimation(matches[k - 1 + mid], nowfeats[k - 1],
				nowfeats[k]).get_transform(&info);
		if (not succ)
			failed = true;
			//error_exit("The two image doesn't match. Failed");
		nowmat[k-1] = info.homo;
	}
	if (failed) return 0;

	REPL(k, 1, len - 1)
		nowmat[k] = nowmat[k - 1].prod(nowmat[k]);	// transform to nowimgs[0] == imgs[mid]

	Vec2D center2 = nowmat.back().trans2d(0, 0);
	const float slope = center2.y/ center2.x;
	print_debug("slope: %lf\n", slope);
	if (update_min(minslope, fabs(slope))) {
		bestfactor = nowfactor;
		mat = move(nowmat);
	}
	return slope;
}

Mat32f Stitcher::perspective_correction(const Mat32f& img) {
	// in warp mode, the last hack
	int w = img.width(), h = img.height();
	int refw = imgs[bundle.identity_idx].width(),
			refh = imgs[bundle.identity_idx].height();
	auto homo2proj = bundle.get_homo2proj();
	Vec2D proj_min = bundle.proj_range.min;

	vector<Vec2D> corners;
	auto cur = &(bundle.component.front());
	auto to_ref_coor = [&](Vec2D v) {
		v.x *= cur->imgptr->width(), v.y *= cur->imgptr->height();
		Vec homo = cur->homo.trans(v);
		homo.x /= refw, homo.y /= refh;
		homo.x += 0.5 * homo.z, homo.y += 0.5 * homo.z;
		Vec2D t_corner = homo2proj(homo);
		t_corner.x *= refw, t_corner.y *= refh;
		t_corner = t_corner - proj_min;
		corners.push_back(t_corner);
	};
	to_ref_coor(Vec2D(-0.5, -0.5));
	to_ref_coor(Vec2D(-0.5, 0.5));
	cur = &(bundle.component.back());
	to_ref_coor(Vec2D(0.5, -0.5));
	to_ref_coor(Vec2D(0.5, 0.5));

	vector<Vec2D> corners_std = {
		Vec2D(0, 0), Vec2D(0, h),
		Vec2D(w, 0), Vec2D(w, h)};
	Matrix m = getPerspectiveTransform(corners, corners_std);
	Homography inv(m);

	LinearBlender blender;
	Mat<Vec2D> orig_pos(h, w, 1);
	REP(i, h) REP(j, w) {
		Vec2D& p = (orig_pos.at(i, j) = inv.trans2d(Vec2D(j, i)));
		if (!p.isNaN() && (p.x < 0 || p.x >= w || p.y < 0 || p.y >= h))
			p = Vec2D::NaN();
	}
	blender.add_image(Coor(0, 0), orig_pos, img);
	auto ret = Mat32f(h, w, 3);
	fill(ret, Color::NO);
	blender.run(ret);
	return ret;
}
