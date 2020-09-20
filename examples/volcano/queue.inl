template <GraphicsBackend B>
QueuePresentInfo<B>& QueuePresentInfo<B>::operator^=(QueuePresentInfo<B>&& other)
{
    waitSemaphores.insert(
        waitSemaphores.end(),
        std::make_move_iterator(other.waitSemaphores.begin()),
        std::make_move_iterator(other.waitSemaphores.end()));
    other.waitSemaphores.clear();
    swapchains.insert(
        swapchains.end(),
        std::make_move_iterator(other.swapchains.begin()),
        std::make_move_iterator(other.swapchains.end()));
    other.swapchains.clear();
    imageIndices.insert(
        imageIndices.end(),
        std::make_move_iterator(other.imageIndices.begin()),
        std::make_move_iterator(other.imageIndices.end()));
    other.imageIndices.clear();
    results.insert(
        results.end(),
        std::make_move_iterator(other.results.begin()),
        std::make_move_iterator(other.results.end()));
    other.results.clear();
    timelineValue = std::max(timelineValue, std::exchange(other.timelineValue, 0));
    return *this;
}
