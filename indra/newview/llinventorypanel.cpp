/* 
 * @file llinventorypanel.cpp
 * @brief Implementation of the inventory panel and associated stuff.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * 
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "llinventorypanel.h"

#include <utility> // for std::pair<>

#include "llagent.h"
#include "llagentwearables.h"
#include "llappearancemgr.h"
#include "llavataractions.h"
#include "llclipboard.h"
#include "llfloaterinventory.h"
#include "llfloaterreg.h"
#include "llfloatersidepanelcontainer.h"
#include "llfolderview.h"
#include "llfolderviewitem.h"
#include "llimfloater.h"
#include "llimview.h"
#include "llinventorybridge.h"
#include "llinventoryfunctions.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llsidepanelinventory.h"
#include "llviewerattachmenu.h"
#include "llviewerfoldertype.h"
#include "llvoavatarself.h"

static LLDefaultChildRegistry::Register<LLInventoryPanel> r("inventory_panel");

const std::string LLInventoryPanel::DEFAULT_SORT_ORDER = std::string("InventorySortOrder");
const std::string LLInventoryPanel::RECENTITEMS_SORT_ORDER = std::string("RecentItemsSortOrder");
const std::string LLInventoryPanel::INHERIT_SORT_ORDER = std::string("");
static const LLInventoryFVBridgeBuilder INVENTORY_BRIDGE_BUILDER;

//
// class LLFolderViewModelInventory
//
static LLFastTimer::DeclareTimer FTM_INVENTORY_SORT("Sort");

void LLFolderViewModelInventory::sort( LLFolderViewFolder* folder )
{
	LLFastTimer _(FTM_INVENTORY_SORT);

	if (!needsSort(folder->getViewModelItem())) return;

	LLFolderViewModelItemInventory* modelp =   static_cast<LLFolderViewModelItemInventory*>(folder->getViewModelItem());
	if (modelp->getUUID().isNull()) return;

	for (std::list<LLFolderViewFolder*>::iterator it =   folder->getFoldersBegin(), end_it = folder->getFoldersEnd();
		it != end_it;
		++it)
	{
		LLFolderViewFolder* child_folderp = *it;
		sort(child_folderp);

		if (child_folderp->getFoldersCount() > 0)
		{
			time_t most_recent_folder_time =
				static_cast<LLFolderViewModelItemInventory*>((*child_folderp->getFoldersBegin())->getViewModelItem())->getCreationDate();
			LLFolderViewModelItemInventory* modelp =   static_cast<LLFolderViewModelItemInventory*>(child_folderp->getViewModelItem());
			if (most_recent_folder_time > modelp->getCreationDate())
			{
				modelp->setCreationDate(most_recent_folder_time);
			}
		}
		if (child_folderp->getItemsCount() > 0)			
		{
			time_t most_recent_item_time =
				static_cast<LLFolderViewModelItemInventory*>((*child_folderp->getItemsBegin())->getViewModelItem())->getCreationDate();

			LLFolderViewModelItemInventory* modelp =   static_cast<LLFolderViewModelItemInventory*>(child_folderp->getViewModelItem());
			if (most_recent_item_time > modelp->getCreationDate())
			{
				modelp->setCreationDate(most_recent_item_time);
			}
		}
	}
	base_t::sort(folder);
}

bool LLFolderViewModelInventory::contentsReady()
{
	return !LLInventoryModelBackgroundFetch::instance().folderFetchActive();
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLInventoryPanelObserver
//
// Bridge to support knowing when the inventory has changed.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

class LLInventoryPanelObserver : public LLInventoryObserver
{
public:
	LLInventoryPanelObserver(LLInventoryPanel* ip) : mIP(ip) {}
	virtual ~LLInventoryPanelObserver() {}
	virtual void changed(U32 mask) 
	{
		mIP->modelChanged(mask);
	}
protected:
	LLInventoryPanel* mIP;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLInvPanelComplObserver
//
// Calls specified callback when all specified items become complete.
//
// Usage:
// observer = new LLInvPanelComplObserver(boost::bind(onComplete));
// inventory->addObserver(observer);
// observer->reset(); // (optional)
// observer->watchItem(incomplete_item1_id);
// observer->watchItem(incomplete_item2_id);
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

class LLInvPanelComplObserver : public LLInventoryCompletionObserver
{
public:
	typedef boost::function<void()> callback_t;

	LLInvPanelComplObserver(callback_t cb)
	:	mCallback(cb)
	{
	}

	void reset();

private:
	/*virtual*/ void done();

	/// Called when all the items are complete.
	callback_t	mCallback;
};

void LLInvPanelComplObserver::reset()
{
	mIncomplete.clear();
	mComplete.clear();
}

void LLInvPanelComplObserver::done()
{
	mCallback();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLInventoryPanel
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

LLInventoryPanel::LLInventoryPanel(const LLInventoryPanel::Params& p) :	
	LLPanel(p),
	mInventoryObserver(NULL),
	mCompletionObserver(NULL),
	mFolderRoot(NULL),
	mScroller(NULL),
	mSortOrderSetting(p.sort_order_setting),
	mInventory(p.inventory),
	mAcceptsDragAndDrop(p.accepts_drag_and_drop),
	mAllowMultiSelect(p.allow_multi_select),
	mShowItemLinkOverlays(p.show_item_link_overlays),
	mShowEmptyMessage(p.show_empty_message),
	mViewsInitialized(false),
	mInvFVBridgeBuilder(NULL)
{
	mInvFVBridgeBuilder = &INVENTORY_BRIDGE_BUILDER;

	// contex menu callbacks
	mCommitCallbackRegistrar.add("Inventory.DoToSelected", boost::bind(&LLInventoryPanel::doToSelected, this, _2));
	mCommitCallbackRegistrar.add("Inventory.EmptyTrash", boost::bind(&LLInventoryModel::emptyFolderType, &gInventory, "ConfirmEmptyTrash", LLFolderType::FT_TRASH));
	mCommitCallbackRegistrar.add("Inventory.EmptyLostAndFound", boost::bind(&LLInventoryModel::emptyFolderType, &gInventory, "ConfirmEmptyLostAndFound", LLFolderType::FT_LOST_AND_FOUND));
	mCommitCallbackRegistrar.add("Inventory.DoCreate", boost::bind(&LLInventoryPanel::doCreate, this, _2));
	mCommitCallbackRegistrar.add("Inventory.AttachObject", boost::bind(&LLInventoryPanel::attachObject, this, _2));
	mCommitCallbackRegistrar.add("Inventory.BeginIMSession", boost::bind(&LLInventoryPanel::beginIMSession, this));
	mCommitCallbackRegistrar.add("Inventory.Share",  boost::bind(&LLAvatarActions::shareWithAvatars));

}

void LLInventoryPanel::buildFolderView(const LLInventoryPanel::Params& params)
{
	// Determine the root folder in case specified, and
	// build the views starting with that folder.
	
	std::string start_folder_name(params.start_folder());
	
	const LLFolderType::EType preferred_type = LLViewerFolderType::lookupTypeFromNewCategoryName(start_folder_name);

	LLUUID root_id;

	if ("LIBRARY" == params.start_folder())
	{
		root_id = gInventory.getLibraryRootFolderID();
	}
	else
	{
		root_id = (preferred_type != LLFolderType::FT_NONE)
				? gInventory.findCategoryUUIDForType(preferred_type, false, false) 
				: LLUUID::null;
	}
	
	if ((root_id == LLUUID::null) && !start_folder_name.empty())
	{
		llwarns << "No category found that matches start_folder: " << start_folder_name << llendl;
		root_id = LLUUID::generateNewID();
	}
	
	LLInvFVBridge* new_listener = mInvFVBridgeBuilder->createBridge(LLAssetType::AT_CATEGORY,
																	LLAssetType::AT_CATEGORY,
																	LLInventoryType::IT_CATEGORY,
																	this,
																	&mInventoryViewModel,
																	NULL,
																	root_id);
	
	mFolderRoot = createFolderView(new_listener, params.use_label_suffix());
	addItemID(root_id, mFolderRoot);
}

void LLInventoryPanel::initFromParams(const LLInventoryPanel::Params& params)
{
	LLMemType mt(LLMemType::MTYPE_INVENTORY_POST_BUILD);

	mCommitCallbackRegistrar.pushScope(); // registered as a widget; need to push callback scope ourselves
	
	buildFolderView(params);

	mCommitCallbackRegistrar.popScope();
	
	mFolderRoot->setCallbackRegistrar(&mCommitCallbackRegistrar);
	
	// Scroller
	LLRect scroller_view_rect = getRect();
	scroller_view_rect.translate(-scroller_view_rect.mLeft, -scroller_view_rect.mBottom);
	LLScrollContainer::Params scroller_params(params.scroll());
	scroller_params.rect(scroller_view_rect);
	mScroller = LLUICtrlFactory::create<LLFolderViewScrollContainer>(scroller_params);
	addChild(mScroller);
	mScroller->addChild(mFolderRoot);
	mFolderRoot->setScrollContainer(mScroller);
	mFolderRoot->setFollowsAll();
	mFolderRoot->addChild(mFolderRoot->mStatusTextBox);

	// Set up the callbacks from the inventory we're viewing, and then build everything.
	mInventoryObserver = new LLInventoryPanelObserver(this);
	mInventory->addObserver(mInventoryObserver);

	mCompletionObserver = new LLInvPanelComplObserver(boost::bind(&LLInventoryPanel::onItemsCompletion, this));
	mInventory->addObserver(mCompletionObserver);

	// Build view of inventory if we need default full hierarchy and inventory ready,
	// otherwise wait for idle callback.
	if (mInventory->isInventoryUsable() && !mViewsInitialized)
	{
		initializeViews();
	}
	gIdleCallbacks.addFunction(onIdle, (void*)this);

	if (mSortOrderSetting != INHERIT_SORT_ORDER)
	{
		setSortOrder(gSavedSettings.getU32(mSortOrderSetting));
	}
	else
	{
		setSortOrder(gSavedSettings.getU32(DEFAULT_SORT_ORDER));
	}

	// hide inbox
	getFilter()->setFilterCategoryTypes(getFilter()->getFilterCategoryTypes() & ~(1ULL << LLFolderType::FT_INBOX));
	getFilter()->setFilterCategoryTypes(getFilter()->getFilterCategoryTypes() & ~(1ULL << LLFolderType::FT_OUTBOX));

	// set the filter for the empty folder if the debug setting is on
	if (gSavedSettings.getBOOL("DebugHideEmptySystemFolders"))
	{
		getFilter()->setFilterEmptySystemFolders();
	}
	
	// keep track of the clipboard state so that we avoid filtering too much
	mClipboardState = LLClipboard::instance().getGeneration();
	
	// Initialize base class params.
	LLPanel::initFromParams(params);
}

LLInventoryPanel::~LLInventoryPanel()
{
	U32 sort_order = getFolderViewModel()->getSorter().getSortOrder();
	if (mSortOrderSetting != INHERIT_SORT_ORDER)
	{
		gSavedSettings.setU32(mSortOrderSetting, sort_order);
	}

	gIdleCallbacks.deleteFunction(onIdle, this);

	// LLView destructor will take care of the sub-views.
	mInventory->removeObserver(mInventoryObserver);
	mInventory->removeObserver(mCompletionObserver);
	delete mInventoryObserver;
	delete mCompletionObserver;

	mScroller = NULL;
}

void LLInventoryPanel::draw()
{
	// Select the desired item (in case it wasn't loaded when the selection was requested)
	updateSelection();
	
	// Nudge the filter if the clipboard state changed
	if (mClipboardState != LLClipboard::instance().getGeneration())
	{
		mClipboardState = LLClipboard::instance().getGeneration();
		getFilter()->setModified(LLClipboard::instance().isCutMode() ? LLInventoryFilter::FILTER_MORE_RESTRICTIVE : LLInventoryFilter::FILTER_LESS_RESTRICTIVE);
	}
	
	LLPanel::draw();
}

const LLInventoryFilter* LLInventoryPanel::getFilter() const
{
	return getFolderViewModel()->getFilter();
}

LLInventoryFilter* LLInventoryPanel::getFilter()
{
	return getFolderViewModel()->getFilter();
}

void LLInventoryPanel::setFilterTypes(U64 types, LLInventoryFilter::EFilterType filter_type)
{
	if (filter_type == LLInventoryFilter::FILTERTYPE_OBJECT)
		getFilter()->setFilterObjectTypes(types);
	if (filter_type == LLInventoryFilter::FILTERTYPE_CATEGORY)
		getFilter()->setFilterCategoryTypes(types);
}

U32 LLInventoryPanel::getFilterObjectTypes() const 
{ 
	return getFilter()->getFilterObjectTypes();
}

U32 LLInventoryPanel::getFilterPermMask() const 
{ 
	return getFilter()->getFilterPermissions();
}


void LLInventoryPanel::setFilterPermMask(PermissionMask filter_perm_mask)
{
	getFilter()->setFilterPermissions(filter_perm_mask);
}

void LLInventoryPanel::setFilterWearableTypes(U64 types)
{
	getFilter()->setFilterWearableTypes(types);
}

void LLInventoryPanel::setFilterSubString(const std::string& string)
{
	getFilter()->setFilterSubString(string);
}

const std::string LLInventoryPanel::getFilterSubString() 
{ 
	return getFilter()->getFilterSubString();
}


void LLInventoryPanel::setSortOrder(U32 order)
{
        LLInventorySort sorter(order);
	getFilter()->setSortOrder(order);
	if (order != getFolderViewModel()->getSorter().getSortOrder())
	{
		getFolderViewModel()->setSorter(LLInventorySort(order));
		// try to keep selection onscreen, even if it wasn't to start with
		mFolderRoot->scrollToShowSelection();
	}
}

U32 LLInventoryPanel::getSortOrder() const 
{ 
	return getFolderViewModel()->getSorter().getSortOrder();
}

void LLInventoryPanel::setSinceLogoff(BOOL sl)
{
	getFilter()->setDateRangeLastLogoff(sl);
}

void LLInventoryPanel::setHoursAgo(U32 hours)
{
	getFilter()->setHoursAgo(hours);
}

void LLInventoryPanel::setFilterLinks(U64 filter_links)
{
	getFilter()->setFilterLinks(filter_links);
}

void LLInventoryPanel::setShowFolderState(LLInventoryFilter::EFolderShow show)
{
	getFilter()->setShowFolderState(show);
}

LLInventoryFilter::EFolderShow LLInventoryPanel::getShowFolderState()
{
	return getFilter()->getShowFolderState();
}

void LLInventoryPanel::modelChanged(U32 mask)
{
	static LLFastTimer::DeclareTimer FTM_REFRESH("Inventory Refresh");
	LLFastTimer t2(FTM_REFRESH);

	bool handled = false;

	if (!mViewsInitialized) return;
	
	const LLInventoryModel* model = getModel();
	if (!model) return;

	const LLInventoryModel::changed_items_t& changed_items = model->getChangedIDs();
	if (changed_items.empty()) return;

	for (LLInventoryModel::changed_items_t::const_iterator items_iter = changed_items.begin();
		 items_iter != changed_items.end();
		 ++items_iter)
	{
		const LLUUID& item_id = (*items_iter);
		const LLInventoryObject* model_item = model->getObject(item_id);
		LLFolderViewItem* view_item = getItemByID(item_id);
		LLFolderViewModelItemInventory* viewmodel_item = 
			static_cast<LLFolderViewModelItemInventory*>(view_item ? view_item->getViewModelItem() : NULL);

		// LLFolderViewFolder is derived from LLFolderViewItem so dynamic_cast from item
		// to folder is the fast way to get a folder without searching through folders tree.
		LLFolderViewFolder* view_folder = dynamic_cast<LLFolderViewFolder*>(view_item);

		//////////////////////////////
		// LABEL Operation
		// Empty out the display name for relabel.
		if (mask & LLInventoryObserver::LABEL)
		{
			handled = true;
			if (view_item)
			{
				// Request refresh on this item (also flags for filtering)
				LLInvFVBridge* bridge = (LLInvFVBridge*)view_item->getViewModelItem();
				if(bridge)
				{	// Clear the display name first, so it gets properly re-built during refresh()
					bridge->clearDisplayName();

					view_item->refresh();
				}
			}
		}

		//////////////////////////////
		// REBUILD Operation
		// Destroy and regenerate the UI.
		if (mask & LLInventoryObserver::REBUILD)
		{
			handled = true;
			if (model_item && view_item)
			{
				view_item->destroyView();
				removeItemID(viewmodel_item->getUUID());
			}
			view_item = buildNewViews(item_id);
			viewmodel_item = 
				static_cast<LLFolderViewModelItemInventory*>(view_item ? view_item->getViewModelItem() : NULL);
			view_folder = dynamic_cast<LLFolderViewFolder *>(view_item);
		}

		//////////////////////////////
		// INTERNAL Operation
		// This could be anything.  For now, just refresh the item.
		if (mask & LLInventoryObserver::INTERNAL)
		{
			if (view_item)
			{
				view_item->refresh();
			}
		}

		//////////////////////////////
		// SORT Operation
		// Sort the folder.
		if (mask & LLInventoryObserver::SORT)
		{
			if (view_folder)
			{
				view_folder->requestSort();
			}
		}	

		// We don't typically care which of these masks the item is actually flagged with, since the masks
		// may not be accurate (e.g. in the main inventory panel, I move an item from My Inventory into
		// Landmarks; this is a STRUCTURE change for that panel but is an ADD change for the Landmarks
		// panel).  What's relevant is that the item and UI are probably out of sync and thus need to be
		// resynchronized.
		if (mask & (LLInventoryObserver::STRUCTURE |
					LLInventoryObserver::ADD |
					LLInventoryObserver::REMOVE))
		{
			handled = true;

			//////////////////////////////
			// ADD Operation
			// Item exists in memory but a UI element hasn't been created for it.
			if (model_item && !view_item)
			{
				// Add the UI element for this item.
				buildNewViews(item_id);
				// Select any newly created object that has the auto rename at top of folder root set.
				if(mFolderRoot->getRoot()->needsAutoRename())
				{
					setSelection(item_id, FALSE);
				}
			}

			//////////////////////////////
			// STRUCTURE Operation
			// This item already exists in both memory and UI.  It was probably reparented.
			else if (model_item && view_item)
			{
				// Don't process the item if it is the root
				if (view_item->getParentFolder())
				{
					LLFolderViewFolder* new_parent =   (LLFolderViewFolder*)getItemByID(model_item->getParentUUID());
					// Item has been moved.
					if (view_item->getParentFolder() != new_parent)
					{
						if (new_parent != NULL)
						{
							// Item is to be moved and we found its new parent in the panel's directory, so move the item's UI.
							view_item->addToFolder(new_parent);
							addItemID(viewmodel_item->getUUID(), view_item);
						}
						else 
						{
							// Item is to be moved outside the panel's directory (e.g. moved to trash for a panel that 
							// doesn't include trash).  Just remove the item's UI.
							view_item->destroyView();
                            removeItemID(viewmodel_item->getUUID());
						}
					}
				}
			}
			
			//////////////////////////////
			// REMOVE Operation
			// This item has been removed from memory, but its associated UI element still exists.
			else if (!model_item && view_item)
			{
				// Remove the item's UI.
				view_item->destroyView();
                removeItemID(viewmodel_item->getUUID());
			}
		}
	}
}

LLFolderView* LLInventoryPanel::getRootFolder() 
{ 
	return mFolderRoot; 
}

LLUUID LLInventoryPanel::getRootFolderID()
{
	return static_cast<LLFolderViewModelItemInventory*>(mFolderRoot->getViewModelItem())->getUUID();
}


// static
void LLInventoryPanel::onIdle(void *userdata)
{
	if (!gInventory.isInventoryUsable())
		return;

	LLInventoryPanel *self = (LLInventoryPanel*)userdata;
	// Inventory just initialized, do complete build
	if (!self->mViewsInitialized)
	{
		self->initializeViews();
	}
	if (self->mViewsInitialized)
	{
		gIdleCallbacks.deleteFunction(onIdle, (void*)self);
	}
}

void LLInventoryPanel::initializeViews()
{
	if (!gInventory.isInventoryUsable()) return;

	rebuildViewsFor(gInventory.getRootFolderID());

	mViewsInitialized = true;
	
	openStartFolderOrMyInventory();
	
	// Special case for new user login
	if (gAgent.isFirstLogin())
	{
		// Auto open the user's library
		LLFolderViewFolder* lib_folder =   getFolderByID(gInventory.getLibraryRootFolderID());
		if (lib_folder)
		{
			lib_folder->setOpen(TRUE);
		}
		
		// Auto close the user's my inventory folder
		LLFolderViewFolder* my_inv_folder =   getFolderByID(gInventory.getRootFolderID());
		if (my_inv_folder)
		{
			my_inv_folder->setOpenArrangeRecursively(FALSE, LLFolderViewFolder::RECURSE_DOWN);
		}
	}
}

LLFolderViewItem* LLInventoryPanel::rebuildViewsFor(const LLUUID& id)
{
	// Destroy the old view for this ID so we can rebuild it.
	LLFolderViewItem* old_view = getItemByID(id);
	if (old_view)
	{
		old_view->destroyView();
		removeItemID(static_cast<LLFolderViewModelItemInventory*>(old_view->getViewModelItem())->getUUID());
	}

	return buildNewViews(id);
}

LLFolderView * LLInventoryPanel::createFolderView(LLInvFVBridge * bridge, bool useLabelSuffix)
{
	LLRect folder_rect(0,
					   0,
					   getRect().getWidth(),
					   0);

	LLFolderView::Params p;
	
	p.name = getName();
	p.title = getLabel();
	p.rect = folder_rect;
	p.parent_panel = this;
	p.tool_tip = p.name;
	p.listener = bridge;
	p.view_model = &mInventoryViewModel;
	p.use_label_suffix = useLabelSuffix;
	p.allow_multiselect = mAllowMultiSelect;
	p.show_empty_message = mShowEmptyMessage;
	p.show_item_link_overlays = mShowItemLinkOverlays;

	return LLUICtrlFactory::create<LLFolderView>(p);
}

LLFolderViewFolder * LLInventoryPanel::createFolderViewFolder(LLInvFVBridge * bridge)
{
	LLFolderViewFolder::Params params;

	params.name = bridge->getDisplayName();
	params.root = mFolderRoot;
	params.listener = bridge;
	params.tool_tip = params.name;

	return LLUICtrlFactory::create<LLFolderViewFolder>(params);
}

LLFolderViewItem * LLInventoryPanel::createFolderViewItem(LLInvFVBridge * bridge)
{
	LLFolderViewItem::Params params;
	
	params.name = bridge->getDisplayName();
	params.creation_date = bridge->getCreationDate();
	params.root = mFolderRoot;
	params.listener = bridge;
	params.rect = LLRect (0, 0, 0, 0);
	params.tool_tip = params.name;
	
	return LLUICtrlFactory::create<LLFolderViewItem>(params);
}

LLFolderViewItem* LLInventoryPanel::buildNewViews(const LLUUID& id)
{
 	LLInventoryObject const* objectp = gInventory.getObject(id);
	
	if (!objectp) return NULL;

	LLFolderViewItem* folder_view_item = getItemByID(id);

	const LLUUID &parent_id = objectp->getParentUUID();
	LLFolderViewFolder* parent_folder = (LLFolderViewFolder*)getItemByID(parent_id);
	 
 	if (!folder_view_item && parent_folder)
  	{
  		if (objectp->getType() <= LLAssetType::AT_NONE ||
  			objectp->getType() >= LLAssetType::AT_COUNT)
  		{
  			llwarns << "LLInventoryPanel::buildNewViews called with invalid objectp->mType : "
  					<< ((S32) objectp->getType()) << " name " << objectp->getName() << " UUID " << objectp->getUUID()
  					<< llendl;
  			return NULL;
  		}
  		
  		if ((objectp->getType() == LLAssetType::AT_CATEGORY) &&
  			(objectp->getActualType() != LLAssetType::AT_LINK_FOLDER))
  		{
  			LLInvFVBridge* new_listener = mInvFVBridgeBuilder->createBridge(objectp->getType(),
  																			objectp->getType(),
  																			LLInventoryType::IT_CATEGORY,
  																			this,
																			&mInventoryViewModel,
  																			mFolderRoot,
  																			objectp->getUUID());
  			if (new_listener)
  			{
				folder_view_item = createFolderViewFolder(new_listener);
  			}
  		}
  		else
  		{
  			// Build new view for item.
  			LLInventoryItem* item = (LLInventoryItem*)objectp;
  			LLInvFVBridge* new_listener = mInvFVBridgeBuilder->createBridge(item->getType(),
  																			item->getActualType(),
  																			item->getInventoryType(),
  																			this,
																			&mInventoryViewModel,
  																			mFolderRoot,
  																			item->getUUID(),
  																			item->getFlags());
 
  			if (new_listener)
  			{
				folder_view_item = createFolderViewItem(new_listener);
  			}
  		}
 
  		if (folder_view_item)
  		{
  			folder_view_item->addToFolder(parent_folder);
			addItemID(id, folder_view_item);
   		}
	}

	// If this is a folder, add the children of the folder and recursively add any 
	// child folders.
	if (folder_view_item && objectp->getType() == LLAssetType::AT_CATEGORY)
	{
		LLViewerInventoryCategory::cat_array_t* categories;
		LLViewerInventoryItem::item_array_t* items;
		mInventory->lockDirectDescendentArrays(id, categories, items);
		
		if(categories)
		{
			for (LLViewerInventoryCategory::cat_array_t::const_iterator cat_iter = categories->begin();
				 cat_iter != categories->end();
				 ++cat_iter)
			{
				const LLViewerInventoryCategory* cat = (*cat_iter);
				buildNewViews(cat->getUUID());
			}
		}
		
		if(items)
		{
			for (LLViewerInventoryItem::item_array_t::const_iterator item_iter = items->begin();
				 item_iter != items->end();
				 ++item_iter)
			{
				const LLViewerInventoryItem* item = (*item_iter);
				buildNewViews(item->getUUID());
			}
		}
		mInventory->unlockDirectDescendentArrays(id);
	}
	
	return folder_view_item;
}

// bit of a hack to make sure the inventory is open.
void LLInventoryPanel::openStartFolderOrMyInventory()
{
	// Find My Inventory folder and open it up by name
	for (LLView *child = mFolderRoot->getFirstChild(); child; child = mFolderRoot->findNextSibling(child))
	{
		LLFolderViewFolder *fchild = dynamic_cast<LLFolderViewFolder*>(child);
		if (fchild
			&& fchild->getViewModelItem()
			&& fchild->getViewModelItem()->getName() == "My Inventory")
		{
			fchild->setOpen(TRUE);
			break;
		}
	}
}

void LLInventoryPanel::onItemsCompletion()
{
	if (mFolderRoot) mFolderRoot->updateMenu();
}

void LLInventoryPanel::openSelected()
{
	LLFolderViewItem* folder_item = mFolderRoot->getCurSelectedItem();
	if(!folder_item) return;
	LLInvFVBridge* bridge = (LLInvFVBridge*)folder_item->getViewModelItem();
	if(!bridge) return;
	bridge->openItem();
}

void LLInventoryPanel::unSelectAll()	
{ 
	mFolderRoot->setSelection(NULL, FALSE, FALSE); 
}


BOOL LLInventoryPanel::handleHover(S32 x, S32 y, MASK mask)
{
	BOOL handled = LLView::handleHover(x, y, mask);
	if(handled)
	{
		ECursorType cursor = getWindow()->getCursor();
		if (LLInventoryModelBackgroundFetch::instance().folderFetchActive() && cursor == UI_CURSOR_ARROW)
		{
			// replace arrow cursor with arrow and hourglass cursor
			getWindow()->setCursor(UI_CURSOR_WORKING);
		}
	}
	else
	{
		getWindow()->setCursor(UI_CURSOR_ARROW);
	}
	return TRUE;
}

BOOL LLInventoryPanel::handleDragAndDrop(S32 x, S32 y, MASK mask, BOOL drop,
								   EDragAndDropType cargo_type,
								   void* cargo_data,
								   EAcceptance* accept,
								   std::string& tooltip_msg)
{
	BOOL handled = FALSE;

	if (mAcceptsDragAndDrop)
	{
		handled = LLPanel::handleDragAndDrop(x, y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);

		// If folder view is empty the (x, y) point won't be in its rect
		// so the handler must be called explicitly.
		// but only if was not handled before. See EXT-6746.
		if (!handled && !mFolderRoot->hasVisibleChildren())
		{
			handled = mFolderRoot->handleDragAndDrop(x, y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
		}

		if (handled)
		{
			mFolderRoot->setDragAndDropThisFrame();
		}
	}

	return handled;
}

void LLInventoryPanel::onFocusLost()
{
	// inventory no longer handles cut/copy/paste/delete
	if (LLEditMenuHandler::gEditMenuHandler == mFolderRoot)
	{
		LLEditMenuHandler::gEditMenuHandler = NULL;
	}

	LLPanel::onFocusLost();
}

void LLInventoryPanel::onFocusReceived()
{
	// inventory now handles cut/copy/paste/delete
	LLEditMenuHandler::gEditMenuHandler = mFolderRoot;

	LLPanel::onFocusReceived();
}

bool LLInventoryPanel::addBadge(LLBadge * badge)
{
	bool badge_added = false;

	if (acceptsBadge())
	{
		badge_added = badge->addToView(mFolderRoot);
	}

	return badge_added;
}

void LLInventoryPanel::openAllFolders()
{
	mFolderRoot->setOpenArrangeRecursively(TRUE, LLFolderViewFolder::RECURSE_DOWN);
	mFolderRoot->arrangeAll();
}

void LLInventoryPanel::setSelection(const LLUUID& obj_id, BOOL take_keyboard_focus)
{
	// Don't select objects in COF (e.g. to prevent refocus when items are worn).
	const LLInventoryObject *obj = gInventory.getObject(obj_id);
	if (obj && obj->getParentUUID() == LLAppearanceMgr::instance().getCOF())
	{
		return;
	}
	setSelectionByID(obj_id, take_keyboard_focus);
}

void LLInventoryPanel::setSelectCallback(const boost::function<void (const std::deque<LLFolderViewItem*>& items, BOOL user_action)>& cb) 
{ 
	if (mFolderRoot) 
	{
		mFolderRoot->setSelectCallback(cb);
	}
}

void LLInventoryPanel::clearSelection()
{
	mSelectThisID.setNull();
}

void LLInventoryPanel::onSelectionChange(const std::deque<LLFolderViewItem*>& items, BOOL user_action)
{
	// Schedule updating the folder view context menu when all selected items become complete (STORM-373).
	mCompletionObserver->reset();
	for (std::deque<LLFolderViewItem*>::const_iterator it = items.begin(); it != items.end(); ++it)
	{
		LLUUID id = static_cast<LLFolderViewModelItemInventory*>((*it)->getViewModelItem())->getUUID();
		LLViewerInventoryItem* inv_item = mInventory->getItem(id);

		if (inv_item && !inv_item->isFinished())
		{
			mCompletionObserver->watchItem(id);
		}
	}

	LLFolderView* fv = getRootFolder();
	if (fv->needsAutoRename()) // auto-selecting a new user-created asset and preparing to rename
	{
		fv->setNeedsAutoRename(FALSE);
		if (items.size()) // new asset is visible and selected
		{
			fv->startRenamingSelectedItem();
		}
	}
}

void LLInventoryPanel::doToSelected(const LLSD& userdata)
{
	mFolderRoot->doToSelected(&gInventory, userdata);
}

void LLInventoryPanel::doCreate(const LLSD& userdata)
{
	menu_create_inventory_item(this, LLFolderBridge::sSelf.get(), userdata);
}

bool LLInventoryPanel::beginIMSession()
{
	std::set<LLFolderViewItem*> selected_items =   mFolderRoot->getSelectionList();

	std::string name;
	static int session_num = 1;

	LLDynamicArray<LLUUID> members;
	EInstantMessage type = IM_SESSION_CONFERENCE_START;

	std::set<LLFolderViewItem*>::const_iterator iter;
	for (iter = selected_items.begin(); iter != selected_items.end(); iter++)
	{

		LLFolderViewItem* folder_item = (*iter);
			
		if(folder_item) 
		{
			LLFolderViewModelItemInventory* fve_listener = static_cast<LLFolderViewModelItemInventory*>(folder_item->getViewModelItem());
			if (fve_listener && (fve_listener->getInventoryType() == LLInventoryType::IT_CATEGORY))
			{

				LLFolderBridge* bridge = (LLFolderBridge*)folder_item->getViewModelItem();
				if(!bridge) return true;
				LLViewerInventoryCategory* cat = bridge->getCategory();
				if(!cat) return true;
				name = cat->getName();
				LLUniqueBuddyCollector is_buddy;
				LLInventoryModel::cat_array_t cat_array;
				LLInventoryModel::item_array_t item_array;
				gInventory.collectDescendentsIf(bridge->getUUID(),
												cat_array,
												item_array,
												LLInventoryModel::EXCLUDE_TRASH,
												is_buddy);
				S32 count = item_array.count();
				if(count > 0)
				{
					//*TODO by what to replace that?
					//LLFloaterReg::showInstance("communicate");

					// create the session
					LLAvatarTracker& at = LLAvatarTracker::instance();
					LLUUID id;
					for(S32 i = 0; i < count; ++i)
					{
						id = item_array.get(i)->getCreatorUUID();
						if(at.isBuddyOnline(id))
						{
							members.put(id);
						}
					}
				}
			}
			else
			{
				LLInvFVBridge* listenerp = (LLInvFVBridge*)folder_item->getViewModelItem();

				if (listenerp->getInventoryType() == LLInventoryType::IT_CALLINGCARD)
				{
					LLInventoryItem* inv_item = gInventory.getItem(listenerp->getUUID());

					if (inv_item)
					{
						LLAvatarTracker& at = LLAvatarTracker::instance();
						LLUUID id = inv_item->getCreatorUUID();

						if(at.isBuddyOnline(id))
						{
							members.put(id);
						}
					}
				} //if IT_CALLINGCARD
			} //if !IT_CATEGORY
		}
	} //for selected_items	

	// the session_id is randomly generated UUID which will be replaced later
	// with a server side generated number

	if (name.empty())
	{
		name = llformat("Session %d", session_num++);
	}

	LLUUID session_id = gIMMgr->addSession(name, type, members[0], members);
	if (session_id != LLUUID::null)
	{
		LLIMFloater::show(session_id);
	}
		
	return true;
}

bool LLInventoryPanel::attachObject(const LLSD& userdata)
{
	// Copy selected item UUIDs to a vector.
	std::set<LLFolderViewItem*> selected_items = mFolderRoot->getSelectionList();
	uuid_vec_t items;
	for (std::set<LLFolderViewItem*>::const_iterator set_iter = selected_items.begin();
		 set_iter != selected_items.end(); 
		 ++set_iter)
	{
		items.push_back(static_cast<LLFolderViewModelItemInventory*>((*set_iter)->getViewModelItem())->getUUID());
	}

	// Attach selected items.
	LLViewerAttachMenu::attachObjects(items, userdata.asString());

	gFocusMgr.setKeyboardFocus(NULL);

	return true;
}

BOOL LLInventoryPanel::getSinceLogoff()
{
	return getFilter()->isSinceLogoff();
}

// DEBUG ONLY
// static 
void LLInventoryPanel::dumpSelectionInformation(void* user_data)
{
	LLInventoryPanel* iv = (LLInventoryPanel*)user_data;
	iv->mFolderRoot->dumpSelectionInformation();
}

BOOL is_inventorysp_active()
{
	LLSidepanelInventory *sidepanel_inventory =	LLFloaterSidePanelContainer::getPanel<LLSidepanelInventory>("inventory");
	if (!sidepanel_inventory || !sidepanel_inventory->isInVisibleChain()) return FALSE;
	return sidepanel_inventory->isMainInventoryPanelActive();
}

// static
LLInventoryPanel* LLInventoryPanel::getActiveInventoryPanel(BOOL auto_open)
{
	S32 z_min = S32_MAX;
	LLInventoryPanel* res = NULL;
	LLFloater* active_inv_floaterp = NULL;

	LLFloater* floater_inventory = LLFloaterReg::getInstance("inventory");
	if (!floater_inventory)
	{
		llwarns << "Could not find My Inventory floater" << llendl;
		return FALSE;
	}

	LLSidepanelInventory *inventory_panel =	LLFloaterSidePanelContainer::getPanel<LLSidepanelInventory>("inventory");

	// Iterate through the inventory floaters and return whichever is on top.
	LLFloaterReg::const_instance_list_t& inst_list = LLFloaterReg::getFloaterList("inventory");
	for (LLFloaterReg::const_instance_list_t::const_iterator iter = inst_list.begin(); iter != inst_list.end(); ++iter)
	{
		LLFloaterSidePanelContainer* inventory_floater = dynamic_cast<LLFloaterSidePanelContainer*>(*iter);
		inventory_panel = inventory_floater->findChild<LLSidepanelInventory>("main_panel");

		if (inventory_floater && inventory_panel && inventory_floater->getVisible())
		{
			S32 z_order = gFloaterView->getZOrder(inventory_floater);
			if (z_order < z_min)
			{
				res = inventory_panel->getActivePanel();
				z_min = z_order;
				active_inv_floaterp = inventory_floater;
			}
		}
	}

	if (res)
	{
		// Make sure the floater is not minimized (STORM-438).
		if (active_inv_floaterp && active_inv_floaterp->isMinimized())
		{
			active_inv_floaterp->setMinimized(FALSE);
		}
	}	
	else if (auto_open)
	{
		floater_inventory->openFloater();

		res = inventory_panel->getActivePanel();
	}

	return res;
}

//static
void LLInventoryPanel::openInventoryPanelAndSetSelection(BOOL auto_open, const LLUUID& obj_id)
{
	LLInventoryPanel *active_panel = LLInventoryPanel::getActiveInventoryPanel(auto_open);

	if (active_panel)
	{
		LL_DEBUGS("Messaging") << "Highlighting" << obj_id  << LL_ENDL;
		
		LLViewerInventoryItem * item = gInventory.getItem(obj_id);
		LLViewerInventoryCategory * cat = gInventory.getCategory(obj_id);
		
		bool in_inbox = false;
		
		LLViewerInventoryCategory * parent_cat = NULL;
		
		if (item)
		{
			parent_cat = gInventory.getCategory(item->getParentUUID());
		}
		else if (cat)
		{
			parent_cat = gInventory.getCategory(cat->getParentUUID());
		}
		
		if (parent_cat)
		{
			in_inbox = (LLFolderType::FT_INBOX == parent_cat->getPreferredType());
		}
		
		if (in_inbox)
		{
			LLSidepanelInventory * sidepanel_inventory =	LLFloaterSidePanelContainer::getPanel<LLSidepanelInventory>("inventory");
			LLInventoryPanel * inventory_panel = NULL;
			
			if (in_inbox)
			{
				sidepanel_inventory->openInbox();
				inventory_panel = sidepanel_inventory->getInboxPanel();
			}

			if (inventory_panel)
			{
				inventory_panel->setSelection(obj_id, TAKE_FOCUS_YES);
			}
		}
		else
		{
			active_panel->setSelection(obj_id, TAKE_FOCUS_YES);
		}
	}
}

void LLInventoryPanel::addHideFolderType(LLFolderType::EType folder_type)
{
	getFilter()->setFilterCategoryTypes(getFilter()->getFilterCategoryTypes() & ~(1ULL << folder_type));
}

BOOL LLInventoryPanel::getIsHiddenFolderType(LLFolderType::EType folder_type) const
{
	return !(getFilter()->getFilterCategoryTypes() & (1ULL << folder_type));
}

void LLInventoryPanel::addItemID( const LLUUID& id, LLFolderViewItem*   itemp )
{
	mItemMap[id] = itemp;
}

void LLInventoryPanel::removeItemID(const LLUUID& id)
{
	LLInventoryModel::cat_array_t categories;
	LLInventoryModel::item_array_t items;
	gInventory.collectDescendents(id, categories, items, TRUE);

	mItemMap.erase(id);

	for (LLInventoryModel::cat_array_t::iterator it = categories.begin(),    end_it = categories.end();
		it != end_it;
		++it)
	{
		mItemMap.erase((*it)->getUUID());
	}

	for (LLInventoryModel::item_array_t::iterator it = items.begin(),   end_it  = items.end();
		it != end_it;
		++it)
	{
		mItemMap.erase((*it)->getUUID());
	}
}

LLFastTimer::DeclareTimer FTM_GET_ITEM_BY_ID("Get FolderViewItem by ID");
LLFolderViewItem* LLInventoryPanel::getItemByID(const LLUUID& id)
{
	LLFastTimer _(FTM_GET_ITEM_BY_ID);

	std::map<LLUUID, LLFolderViewItem*>::iterator map_it;
	map_it = mItemMap.find(id);
	if (map_it != mItemMap.end())
	{
		return map_it->second;
	}

	return NULL;
}

LLFolderViewFolder* LLInventoryPanel::getFolderByID(const LLUUID& id)
{
	LLFolderViewItem* item = getItemByID(id);
	return dynamic_cast<LLFolderViewFolder*>(item);
}


void LLInventoryPanel::setSelectionByID( const LLUUID& obj_id, BOOL    take_keyboard_focus )
{
	LLFolderViewItem* itemp = getItemByID(obj_id);
	if(itemp && itemp->getViewModelItem())
	{
		itemp->arrangeAndSet(TRUE, take_keyboard_focus);
		mSelectThisID.setNull();
		return;
	}
	else
	{
		// save the desired item to be selected later (if/when ready)
		mSelectThisID = obj_id;
	}
}

void LLInventoryPanel::updateSelection()
{
	if (mSelectThisID.notNull())
	{
		setSelectionByID(mSelectThisID, false);
	}
}


/************************************************************************/
/* Recent Inventory Panel related class                                 */
/************************************************************************/
class LLInventoryRecentItemsPanel;
static LLDefaultChildRegistry::Register<LLInventoryRecentItemsPanel> t_recent_inventory_panel("recent_inventory_panel");

static const LLRecentInventoryBridgeBuilder RECENT_ITEMS_BUILDER;
class LLInventoryRecentItemsPanel : public LLInventoryPanel
{
public:
	struct Params :	public LLInitParam::Block<Params, LLInventoryPanel::Params>
	{};

	void initFromParams(const Params& p)
	{
		LLInventoryPanel::initFromParams(p);
		// turn on inbox for recent items
		getFilter()->setFilterCategoryTypes(getFilter()->getFilterCategoryTypes() | (1ULL << LLFolderType::FT_INBOX));
	}

protected:
	LLInventoryRecentItemsPanel (const Params&);
	friend class LLUICtrlFactory;
};

LLInventoryRecentItemsPanel::LLInventoryRecentItemsPanel( const Params& params)
: LLInventoryPanel(params)
{
	// replace bridge builder to have necessary View bridges.
	mInvFVBridgeBuilder = &RECENT_ITEMS_BUILDER;
}


void LLFolderViewModelItemInventory::requestSort()
{
	LLFolderViewModelItemCommon::requestSort();
	if (mRootViewModel->getSorter().isByDate())
	{
		// sort by date potentially affects parent folders which use a date
		// derived from newest item in them
		if (mParent)
		{
			mParent->requestSort();
		}
	}
}

bool LLFolderViewModelItemInventory::potentiallyVisible()
{
	return passedFilter() // we've passed the filter
		|| getLastFilterGeneration() < mRootViewModel->getFilter()->getFirstSuccessGeneration() // or we don't know yet
		|| descendantsPassedFilter();
}

bool LLFolderViewModelItemInventory::passedFilter(S32 filter_generation) 
{ 
	if (filter_generation < 0) filter_generation = mRootViewModel->getFilter()->getFirstSuccessGeneration();
	return mPassedFolderFilter 
		&& mLastFilterGeneration >= filter_generation
		&& (mPassedFilter || descendantsPassedFilter(filter_generation));
}

bool LLFolderViewModelItemInventory::descendantsPassedFilter(S32 filter_generation)
{ 
	if (filter_generation < 0) filter_generation = mRootViewModel->getFilter()->getFirstSuccessGeneration();
	return mMostFilteredDescendantGeneration >= filter_generation; 
}

void LLFolderViewModelItemInventory::setPassedFilter(bool passed, bool passed_folder, S32 filter_generation)
{
	mPassedFilter = passed;
	mPassedFolderFilter = passed_folder;
	mLastFilterGeneration = filter_generation;
}

void LLFolderViewModelItemInventory::filterChildItem( LLFolderViewModelItem* item, LLFolderViewFilter& filter )
{
	S32 filter_generation = filter.getCurrentGeneration();
	S32 must_pass_generation = filter.getFirstRequiredGeneration();

	// mMostFilteredDescendantGeneration might have been reset
	// in which case we need to update it even for folders that
	// don't need to be filtered anymore
	if (item->getLastFilterGeneration() < filter_generation)
	{
		if (item->getLastFilterGeneration() >= must_pass_generation && 
			!item->passedFilter(must_pass_generation))
		{
			// failed to pass an earlier filter that was a subset of the current one
			// go ahead and flag this item as done
			item->setPassedFilter(false, false, filter_generation);
		}
		else
		{
			//TODO RN:
			item->filter( filter );
		}
	}

	// track latest generation to pass any child items
	if (item->passedFilter())
	{
		mMostFilteredDescendantGeneration = filter_generation;
		//TODO RN: ensure this still happens
		//requestArrange();
	}
}

void LLFolderViewModelItemInventory::filter( LLFolderViewFilter& filter)
{
	if(!mChildren.empty()
		&& (getLastFilterGeneration() < filter.getFirstRequiredGeneration() // haven't checked descendants against minimum required generation to pass
			|| descendantsPassedFilter(filter.getFirstRequiredGeneration()))) // or at least one descendant has passed the minimum requirement
	{
		// now query children
		for (child_list_t::iterator iter = mChildren.begin();
			iter != mChildren.end() && filter.getFilterCount() > 0;
			++iter)
		{
			filterChildItem((*iter), filter);
		}
	}

	// if we didn't use all filter iterations
	// that means we filtered all of our descendants
	// so filter ourselves now
	if (filter.getFilterCount() > 0)
	{
		const BOOL previous_passed_filter = mPassedFilter;
		const BOOL passed_filter = filter.check(this);
		const BOOL passed_filter_folder = (getInventoryType() == LLInventoryType::IT_CATEGORY) 
								? filter.checkFolder(this)
								: true;

		// If our visibility will change as a result of this filter, then
		// we need to be rearranged in our parent folder
		LLFolderViewFolder* parent_folder = mFolderViewItem->getParentFolder();
		if (parent_folder && passed_filter != previous_passed_filter)
		{
			parent_folder->requestArrange();
		}
	
		setPassedFilter(passed_filter, passed_filter_folder, filter.getCurrentGeneration());
		//TODO RN: create interface for string highlighting
		//mStringMatchOffset = filter.getStringMatchOffset(this);
		filter.decrementFilterCount();
	}
}

LLFolderViewModelInventory* LLInventoryPanel::getFolderViewModel()
{
	return &mInventoryViewModel;
}


const LLFolderViewModelInventory* LLInventoryPanel::getFolderViewModel() const
{
	return &mInventoryViewModel;
}

