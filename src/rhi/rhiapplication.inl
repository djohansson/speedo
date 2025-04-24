namespace rhiapplication
{

template <typename LoadOp>
void OpenFileDialogueAsync(std::string&& resourcePathString, const std::vector<nfdu8filteritem_t>& filterList, LoadOp loadOp)
{
	auto app = std::static_pointer_cast<RHIApplication>(Application::Get().lock());
	ENSURE(app);
	auto& rhi = app->GetRHI();
	
	auto [openFileTask, openFileFuture] = CreateTask(
		window::OpenFileDialogue,
		std::move(resourcePathString),
		filterList);

	auto [loadTask, loadFuture] = CreateTask(
		[](auto openFileFuture, auto loadOp)
		{
			ZoneScopedN("RHIApplication::draw::loadTask");

			ASSERT(openFileFuture.Valid());
			ASSERT(openFileFuture.IsReady());

			auto [openFileResult, openFilePath] = openFileFuture.Get();
			if (openFileResult)
			{
				RHIApplication::gProgress = 0;
				RHIApplication::gShowProgress = true;
				loadOp(openFilePath, RHIApplication::gProgress);
				RHIApplication::gShowProgress = false;
			}
		},
		std::move(openFileFuture),
		loadOp);

	rhi.mainCalls.enqueue(openFileTask);
	rhi.mainCalls.enqueue(loadTask);
}

} // namespace rhiapplication
