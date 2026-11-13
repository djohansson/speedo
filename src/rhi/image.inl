namespace rhi
{

// Image's and ImageView's Swap and moves: the same for every backend

template <GraphicsApi G>
void Image<G>::Swap(Image& rhs) noexcept
{
	DeviceObject<Image<G>>::Swap(rhs);
	std::swap(myImage, rhs.myImage);
}

template <GraphicsApi G>
Image<G>::Image(Image&& other) noexcept
{
	Swap(other);
}

template <GraphicsApi G>
Image<G>& Image<G>::operator=(Image&& other) noexcept
{
	Swap(other);
	return *this;
}

template <GraphicsApi G>
void ImageView<G>::Swap(ImageView& rhs) noexcept
{
	DeviceObject<ImageView<G>>::Swap(rhs);
	std::swap(myView, rhs.myView);
}

template <GraphicsApi G>
ImageView<G>::ImageView(ImageView&& other) noexcept
{
	Swap(other);
}

template <GraphicsApi G>
ImageView<G>& ImageView<G>::operator=(ImageView&& other) noexcept
{
	Swap(other);
	return *this;
}

} // namespace rhi
