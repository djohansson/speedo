template <FrustumPlane Plane>
glm::vec4 Camera::GetFrustumPlane() const noexcept
{
	glm::vec4 result;
	
	if constexpr (Plane == FrustumPlane::Left)
		result = myProjectionMatrix[3] + myProjectionMatrix[0];
	else if constexpr (Plane == FrustumPlane::Right)
		result = myProjectionMatrix[3] - myProjectionMatrix[0];
	else if constexpr (Plane == FrustumPlane::Bottom)
		result = myProjectionMatrix[3] + myProjectionMatrix[1];
	else if constexpr (Plane == FrustumPlane::Top)
		result = myProjectionMatrix[3] - myProjectionMatrix[1];
	else if constexpr (Plane == FrustumPlane::Back)
		result = myProjectionMatrix[3] + myProjectionMatrix[2];
	else if constexpr (Plane == FrustumPlane::Front)
		result = myProjectionMatrix[3] - myProjectionMatrix[2];

	return result;
}
