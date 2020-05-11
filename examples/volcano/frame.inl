template <GraphicsBackend B>
Frame<B>::Frame(Frame<B>&& other)
: BaseType(std::move(other))
, myFence(other.myFence)
, myRenderCompleteSemaphore(other.myRenderCompleteSemaphore)
, myNewImageAcquiredSemaphore(other.myNewImageAcquiredSemaphore)
{
    other.myFence = 0;
    other.myRenderCompleteSemaphore = 0;
    other.myNewImageAcquiredSemaphore = 0;
}
