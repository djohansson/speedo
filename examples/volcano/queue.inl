template <GraphicsBackend B>
QueuePresentInfo<B>& QueuePresentInfo<B>::operator^=(QueuePresentInfo<B>&& other)
{
    waitSemaphores.insert(
        waitSemaphores.end(),
        std::make_move_iterator(other.waitSemaphores.begin()),
        std::make_move_iterator(other.waitSemaphores.end()));
    swapchains.insert(
        swapchains.end(),
        std::make_move_iterator(other.swapchains.begin()),
        std::make_move_iterator(other.swapchains.end()));
    imageIndices.insert(
        imageIndices.end(),
        std::make_move_iterator(other.imageIndices.begin()),
        std::make_move_iterator(other.imageIndices.end()));
    results.insert(
        results.end(),
        std::make_move_iterator(other.results.begin()),
        std::make_move_iterator(other.results.end()));
    timelineValue = std::max(timelineValue, std::exchange(other.timelineValue, 0));
    return *this;
}
