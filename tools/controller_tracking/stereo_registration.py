"""Metric stereo registration from compatible LED triangles (experimental offline path)."""
import itertools
import numpy as np
from pose_geometry import rotation_distance
from bootstrap_model import epipolar_score, triangulate


def rigid_fit(source, target):
    """Least-squares proper rigid transform; no fitted scale."""
    a, b = np.asarray(source), np.asarray(target)
    ca, cb = a.mean(0), b.mean(0)
    u, _, vt = np.linalg.svd((a-ca).T @ (b-cb))
    R = vt.T @ np.diag([1, 1, np.linalg.det(vt.T @ u.T)]) @ u.T
    return R, cb - R @ ca


def stereo_cloud(da, db, TA, TB, epipolar_gate=.005, gap_gate=.005):
    a, b = da @ TA[:3,:3].T, db @ TB[:3,:3].T
    points, identities = [], []
    for i, ray in enumerate(a):
        for j in np.flatnonzero(epipolar_score(TA[:3,3],ray,TB[:3,3],b) <= epipolar_gate):
            point, gap = triangulate(TA[:3,3],ray,TB[:3,3],b[j])
            if point is None or gap > gap_gate:
                continue
            if np.dot(point-TA[:3,3],ray) <= 0 or np.dot(point-TB[:3,3],b[j]) <= 0:
                continue
            points.append(point); identities.append((i,int(j)))
    return np.asarray(points).reshape(-1,3), identities


def unique_matches(predicted, cloud, identities, gate=.012):
    distance = np.linalg.norm(predicted[:,None]-cloud[None],axis=2)
    used_model, used_a, used_b = set(), set(), set()
    matches = []
    for index in np.argsort(distance,axis=None):
        i,j = np.unravel_index(index,distance.shape)
        if distance[i,j] > gate:
            break
        a,b = identities[j]
        if i in used_model or a in used_a or b in used_b:
            continue
        used_model.add(i);used_a.add(a);used_b.add(b);matches.append((i,j))
    error = float(np.mean([distance[i,j] for i,j in matches])) if matches else float('inf')
    return matches,error


def register(model, cloud, identities, distance_gate=.008, match_gate=.012, minimum=4, finalize=None, hypothesis_sink=None, match_filter=None):
    """Find compatible triangle seeds, then rigidly refine unique metric matches.

    No model subset prefix, blob-area ranking, external pose or reference trajectory.
    Thresholds are experimental and do not provide calibrated confidence.
    """
    if len(cloud)<3 or len(model)<3:
        return None, {'triangles':0,'candidates':0}
    ordered = np.asarray(list(itertools.permutations(range(len(model)),3)))
    mp = model[ordered]
    md = np.stack([np.linalg.norm(mp[:,0]-mp[:,1],axis=1),
                   np.linalg.norm(mp[:,0]-mp[:,2],axis=1),
                   np.linalg.norm(mp[:,1]-mp[:,2],axis=1)],axis=1)
    best=None;shortlist=[];stats={'triangles':0,'candidates':0}
    for triple in itertools.combinations(range(len(cloud)),3):
        pairs=[identities[i] for i in triple]
        if len({a for a,b in pairs})<3 or len({b for a,b in pairs})<3: continue
        target=cloud[list(triple)]
        if np.linalg.norm(np.cross(target[1]-target[0],target[2]-target[0])) < 1e-5: continue
        td=np.array([np.linalg.norm(target[0]-target[1]),np.linalg.norm(target[0]-target[2]),np.linalg.norm(target[1]-target[2])])
        compatible=np.flatnonzero(np.max(np.abs(md-td),axis=1) <= distance_gate)
        stats['triangles']+=1
        for k in compatible:
            R,t=rigid_fit(mp[k],target)
            matches,error=unique_matches(model@R.T+t,cloud,identities,match_gate)
            if match_filter is not None:matches=match_filter(R,t,matches)
            if len(matches)<minimum: continue
            for _ in range(2):
                mi,ci=zip(*matches);R,t=rigid_fit(model[list(mi)],cloud[list(ci)])
                matches,error=unique_matches(model@R.T+t,cloud,identities,match_gate)
                if match_filter is not None:matches=match_filter(R,t,matches)
                if len(matches)<minimum: break
            if len(matches)<minimum:continue
            stats['candidates']+=1
            score=(-len(matches),error)
            if best is None or score<best[0]:best=(score,R,t,matches)
            if finalize is not None:
                # Deduplicate correspondence sets before costly joint-camera refinement.
                key=tuple(sorted(matches))
                if not any(item[4]==key or (np.linalg.norm(item[2]-t)<.002 and rotation_distance(item[1],R)<.08) for item in shortlist):
                    shortlist.append((score,R,t,matches,key))
                    shortlist.sort(key=lambda item:item[0])
                    del shortlist[64:]
    if finalize is not None:
        refined=[]
        for _,R,t,matches,_ in shortlist:
            result=finalize(R,t,matches)
            if result is not None:
                refined.append(result)
                if hypothesis_sink is not None:hypothesis_sink(result)
        stats['finalists']=len(shortlist)
        stats['accepted_finalists']=len(refined)
        if not refined:return None,stats
        refined.sort(key=lambda item:item[0])
        _,R,t,error,matches=refined[0]
        return (R,t,error,matches),stats
    if best is None:return None,stats
    return (best[1],best[2],best[0][1],best[3]),stats


def stereo_residual_jacobian(source,R,t,observations,transforms):
    """Normalized-image residual and analytic Jacobian for left rotation increments."""
    rotated=source@R.T
    skew=np.zeros((len(source),3,3))
    x,y,z=rotated.T
    skew[:,0,1]=-z;skew[:,0,2]=y
    skew[:,1,0]=z;skew[:,1,2]=-x
    skew[:,2,0]=-y;skew[:,2,1]=x
    errors=[];jacobians=[]
    for T,obs in zip(transforms,observations):
        camera_rotation=T[:3,:3].T
        points=(rotated+t-T[:3,3])@camera_rotation.T
        if np.any(points[:,2]<=1e-9):return None,None
        px,py,pz=points.T
        projection=np.zeros((len(source),2,3))
        projection[:,0,0]=1/pz;projection[:,1,1]=1/pz
        projection[:,0,2]=-px/(pz*pz);projection[:,1,2]=-py/(pz*pz)
        world_projection=projection@camera_rotation
        jacobians.append(np.concatenate([world_projection@(-skew),world_projection],axis=2).reshape(-1,6))
        errors.append((points[:,:2]/points[:,2:3]-obs[:,:2]/obs[:,2:3]).ravel())
    return np.concatenate(errors),np.concatenate(jacobians)


def refine_stereo(model,R,t,matches,identities,da,db,TA,TB,iterations=8):
    """Joint normalized-image least squares with fixed stereo LED identities.

    Robust weights limit individual residual influence; support must be checked after fitting.
    """
    import cv2
    mi,ci=zip(*matches)
    source=model[list(mi)]
    pairs=[identities[i] for i in ci]
    observations=[da[[i for i,j in pairs]],db[[j for i,j in pairs]]]
    R,t=R.copy(),t.copy()
    for _ in range(iterations):
        r,J=stereo_residual_jacobian(source,R,t,observations,(TA,TB))
        if r is None:return None
        weights=np.sqrt(np.minimum(1,.005/np.maximum(abs(r),1e-12)))
        step=np.linalg.lstsq(J*weights[:,None],-r*weights,rcond=None)[0]
        if not np.isfinite(step).all():return None
        if np.linalg.norm(step)>1:return None
        accepted=False
        for alpha in (1.,.5,.25,.125):
            newR=cv2.Rodrigues(alpha*step[:3])[0]@R;newt=t+alpha*step[3:]
            newr,_=stereo_residual_jacobian(source,newR,newt,observations,(TA,TB))
            if newr is not None and np.linalg.norm(newr*weights)<=np.linalg.norm(r*weights):
                R,t=newR,newt;accepted=True;break
        if not accepted or np.linalg.norm(step)<1e-8:break
    return R,t
