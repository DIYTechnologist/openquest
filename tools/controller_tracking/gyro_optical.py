"""Fast gyro-predicted optical refinement. Different cameras may see different LEDs."""
import cv2
import numpy as np
from stereo_registration import stereo_residual_jacobian


def match_views(model,normals,R,t,views,gate=.03):
    world=model@R.T+t;normal_world=normals@R.T if normals is not None else None
    matches=[]
    for T,bearings in views:
        if len(bearings)==0:continue
        camera=(world-T[:3,3])@T[:3,:3];valid=camera[:,2]>1e-9
        if normal_world is not None:
            ray=T[:3,3]-world
            valid &= np.sum(ray*normal_world,axis=1)>=-.0872*np.linalg.norm(ray,axis=1)
        ids=np.flatnonzero(valid)
        observed=np.flatnonzero(bearings[:,2]>1e-9)
        if not len(ids) or not len(observed):continue
        pred=camera[ids,:2]/camera[ids,2:3];obs=bearings[observed,:2]/bearings[observed,2:3]
        distance=np.linalg.norm(pred[:,None]-obs[None],axis=2);pairs=[];used_model=set();used_obs=set()
        for index in np.argsort(distance,axis=None):
            i,j=np.unravel_index(index,distance.shape)
            if distance[i,j]>gate:break
            if i in used_model or j in used_obs:continue
            used_model.add(i);used_obs.add(j);pairs.append((int(ids[i]),int(observed[j])))
        if pairs:matches.append((T,bearings,pairs))
    return matches


def translation_fit(model,R,matches):
    A=[];b=[]
    for T,rays,pairs in matches:
        for mi,bi in pairs:
            ray=T[:3,:3]@rays[bi];ray=ray/np.linalg.norm(ray)
            projector=np.eye(3)-np.outer(ray,ray)
            A.append(projector);b.append(projector@(T[:3,3]-R@model[mi]))
    if not A:return None
    A=np.vstack(A);b=np.concatenate(b);s=np.linalg.svd(A,compute_uv=False)
    if s[-1]<=0 or s[0]/s[-1]>2000:return None
    return np.linalg.lstsq(A,b,rcond=None)[0]


def residual_jacobian(model,R,t,matches):
    errors=[];jacobians=[]
    for T,rays,pairs in matches:
        mi,bi=zip(*pairs)
        r,J=stereo_residual_jacobian(model[list(mi)],R,t,[rays[list(bi)]],[T])
        if r is None:return None,None
        errors.append(r);jacobians.append(J)
    if not errors:return None,None
    return np.concatenate(errors),np.concatenate(jacobians)


def refine_optical(model,normals,predicted_R,predicted_t,views,angular_weight=.2,diagnostics=None):
    R,t=predicted_R.copy(),predicted_t.copy();matches=[]
    def failure(reason):
        if diagnostics is not None:diagnostics['reason']=reason
        return None
    for outer in range(3):
        # Broad acquisition first; reject inconsistent associations before final fit.
        matches=match_views(model,normals,R,t,views,gate=(.03,.015,.006)[outer])
        unique={mi for T,rays,pairs in matches for mi,bi in pairs}
        if diagnostics is not None:diagnostics.update(unique_leds=len(unique),per_view_matches=[len(pairs) for T,rays,pairs in matches],outer=outer)
        if len(unique)<3:return failure('insufficient_matches')
        fitted=translation_fit(model,R,matches)
        if fitted is None:return failure('ill_conditioned_translation')
        t=fitted
        for _ in range(4):
            r,J=residual_jacobian(model,R,t,matches)
            if r is None:return failure('behind_camera')
            prior=cv2.Rodrigues(R@predicted_R.T)[0].ravel()
            # Small-angle prior is valid only within the explicit 0.2-radian trust region.
            if np.linalg.norm(prior)>.2:return failure('gyro_trust_region')
            prior_J=np.hstack([np.eye(3)*angular_weight,np.zeros((3,3))])
            weight=np.sqrt(np.minimum(1,.005/np.maximum(np.abs(r),1e-12)))
            step=np.linalg.lstsq(np.vstack([J*weight[:,None],prior_J]),-np.r_[r*weight,angular_weight*prior],rcond=None)[0]
            if not np.isfinite(step).all() or np.linalg.norm(step)>.5:return failure('unstable_step')
            R=cv2.Rodrigues(step[:3])[0]@R;t=t+step[3:]
            if np.linalg.norm(step)<1e-7:break
    r,_=residual_jacobian(model,R,t,matches)
    error=float(np.mean(np.linalg.norm(r.reshape(-1,2),axis=1)))
    if diagnostics is not None:diagnostics['error']=error
    if error>.006:return failure('reprojection')
    return R,t,error,len({mi for T,rays,pairs in matches for mi,bi in pairs})
