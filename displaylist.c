/** ***************************************************************************
 * Description       : Display List for VBIT teletext inserter
 * Compiler          : GCC
 *
 * Copyright (C) 2012, Peter Kwan
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice itand thisboard
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaims all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, data or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 ***************************************************************************
 * Hardware : The platform is a Mattair JD200 or MT-X1 XMega development board.
 *
 * Display List Manager
 * ====================
 * The display list controls the order of page transmission.
 * The display list:
 * Lives in the SRAM therefore it must be constructed at the start of each run
 * Uses a linked list.
 * Handles page updates such as add, replace, remove.
 * Maintains a sorted list of pages
 * Has special nodes for carousels and dynamically generated pages.
 * The display list points to the pages.all file and the page index.
 * Maintains magazine lists for parallel transmission.
 * 
 * Each node is a fixed size structure which contains:
 * Page pointer
 * Next node
 * Page
 * Subpage 
 * Node type
 *
 * The page is a pointer [or an index?] to a page
 * Next node indexes the next node in the display list.
 * Magazine is 1..8 [If we ran separate mag lists then the mag would be implied]
 * Page is 0x00..0xFF
 * Subpage is 00 to 99
 * Node type can be N=normal, J=junction, 0=null C=carousel list.
 * A junction node is created if there is more than one subpage. It is used for carousels.
 * A null is used for the last item in a list. There should be a null at the end of each magazine.
 * However, a carousel list needs to be looped.
 * Unused nodes are cleared of data and placed in a free list.
 *
 * How are carousels handled?
 * The Junction node represents a page. It points to a list of the subpages in that page.
 * The subpages form a circular list. The current subpage is pointed to in the Junction node.
 * There is a sub-list used for carousels. Each item in the carousel list points to the junction and
 * has a count down timer. 
 * Anyway. When a carousel page is put up we need to push it to a carousel list.
 * Do we need one for each mag? Probably not. This can be another list of nodes
 * Each node has a pointer to its J node and a timer. If the timer reaches zero then
 * the carousel is flagged to be transmitted. The J node is loaded. The page extracted,
 * and the next page pointer loaded, the carousel list timer reset, (or deleted if the carousel is gone),
 * the J node updated with the next carousel page and finally the state machine primed to
 * transmit the page.
 * The carousel will also be refreshed in the normal transmission cycle. In this case it just
 * retransmits the page that it is currently pointing to.
 *
 * 
 * Note that we can't do true parallel transmission because we would need more file handles
 * than we can afford, given 8k of RAM.
 
 
 */
 
 #include "displaylist.h"
 
 static NODEPTR sFreeList; // FreeList is an index to the DisplayList. It points to the first free node.
 static NODEPTR sDisplayList; // Root of the display list
 
 /* GetNodePtr and SetNodePtr get NODEPTR values from PageArray */
 /** Write nodeptr to slot addr
  *  \param addr - Address in serial ram
  *  \param nodeptr - Value to set
  */
 void SetNodePtr(NODEPTR nodeptr, uint16_t addr)
 {
	// xprintf(PSTR("[SetNodePtr]Writing node pointer %04X to addr %d \n\r "),nodeptr,addr);
	SetSPIRamAddress(SPIRAM_WRITE, addr); // Set the address
	WriteSPIRam((char*)&nodeptr, sizeof(NODEPTR)); // Write data
	DeselectSPIRam();	// let go
 } // SetNodePtr
 
 /* Fetch a node from the PageArray in serial ram
  * The address should be calculated like this: cellAddress=(((mag-1)<<8)+page)*sizeof(NODEPTR);
  */ 
 NODEPTR GetNodePtr(uint16_t *addr)
 {
	NODEPTR nodeptr;
	// xprintf(PSTR("[GetNodePtr]Reading node pointer (%d bytes) from %d \n\r "),sizeof(NODEPTR),addr);
	SetSPIRamAddress(SPIRAM_READ, *addr); // Set the address
	ReadSPIRam((char *)&nodeptr, sizeof(NODEPTR)); // Read data
	DeselectSPIRam();
	//xprintf(PSTR("[GetNodePtr] returns nodeptr=%d\n\r"),nodeptr);
	return nodeptr;
 } // GetNodePtr
 
 /* Write node to slot i in the serial ram */
 void SetNode(DISPLAYNODE *node, NODEPTR i)
 {
	uint8_t subpage;
	subpage=node->subpage;
	i=i*sizeof(DISPLAYNODE)+PAGEARRAYSIZE;	// Find the actual serial ram address
	// TODO MAYBE. Check that i is less than MAXSRAM
	// put out all the values
	SetSPIRamAddress(SPIRAM_WRITE, i); // Write this node
	WriteSPIRam((char*)node, sizeof(DISPLAYNODE)); // Assuming data is in same order as declaration with no byte alignment padding
	DeselectSPIRam();
	//if (subpage!=FREENODE)
	//	xprintf(PSTR("[SetNode] addr=%d subpage=%d\n\r"),i,subpage);
	
 } // SetNode
 
 /* Fetch a node from the slot in serial ram
  * Would this be better being passed by reference?
  */ 
 void GetNode(DISPLAYNODE *node,NODEPTR i)
 {
	i=i*sizeof(DISPLAYNODE)+PAGEARRAYSIZE;	// Find the actual serial ram address
	SetSPIRamAddress(SPIRAM_READ, i); // Write this node
	ReadSPIRam((char *)node, sizeof(DISPLAYNODE));
	DeselectSPIRam();
 } // GetNode
 
 void DumpNode(NODEPTR np)
 {
	DISPLAYNODE n;
	GetNode(&n,np);
	xprintf(PSTR("Node (%d) pageindex=%d next=%d subpage=%d\n\r"),np,n.pageindex,n.next,n.subpage);
 } // DumpNode

 
 static void Dump(void)
 {
	uint16_t i;
		xprintf(PSTR("[Dump] ... \n\r"));	
	for (i=0;i<10;i++)
		DumpNode(i);
	for (i=0;i<20;i+=2)
		xprintf(PSTR("nodeptr[%d]=%d\n\r"),i,GetNodePtr(&i));
 }
 
 /** Grab a node from the free list
  * \return a node pointer
  */
 NODEPTR NewNode(void)
 {
	NODEPTR ix=sFreeList;	// The first node in the free list is what we are going to grab
	DISPLAYNODE node;
	GetNode(&node,sFreeList); // So we need to update the FreeList pointer
	// TODO: Check that we didn't empty the free list
	if (node.subpage==NULLNODE)
	{
		// TODO: Oh dear. What can we do now? We are out of nodes
		xprintf(PSTR("[NewNode] NULLNODE error\n\r"));
	}
	else
		sFreeList=node.next;
	// xprintf(PSTR("[NewNode] Returns %d, Freelist=%d\n\r"),ix,sFreeList);
	return ix;
 } // NewNode

 /** Given a serial ram slot i, 
  * It clears out slot i and links it into the free list
  * We probably should call this from initDisplayList too as the code is duplicated
  * WARNING. You must unlink this node or the display list will get chopped
  */
 void ReturnToFreeList(NODEPTR i)
 {
	DISPLAYNODE node;
	// Set the values in this node
 	node.pageindex=0;
	node.subpage=FREENODE;
	node.next=sFreeList; // This node points to the rest of the list
	SetNode(&node,i);	// TODO: Check that i is in range
	sFreeList=i;		// And the free list now points to this node
 } // ReturnToFreeList
 
 /* Sets the initial value of all the display list slots
  * and joins them together into a freelist.
  */
 void MakeFreeList(void)
 {
	int i;
	DISPLAYNODE node; 
	xprintf(PSTR("Page Array size is %d \n\r"),PAGEARRAYSIZE);
	xprintf(PSTR("Display list can contain up to %d nodes \n\r"),MAXNODES);
	sFreeList=0;
	// We need to make one node to start with
	node.pageindex=0;
	node.next=0;
	node.subpage=NULLNODE;
	SetNode(&node,0);
	for (i=MAXNODES-1;i>=0;i--)
	{
		if (i%100==0) xprintf(PSTR("M"));
		ReturnToFreeList(i);
	}
xprintf(PSTR("\n\r"));		
	// The FreeList is now ready
	// But we now have to clear out the PageArray
	for (i=0;i<PAGEARRAYSIZE;i+=sizeof(NODEPTR))
	{
		if (i%100==0)
			xprintf(PSTR("P"));
		SetNodePtr(NULLPTR, i);		
	}
xprintf(PSTR("\n\r"));		
 } // MakeFreeList
 
 /** Insert a page into the display list
  * \param mag - 1..8
  * \param page - pointer to a page structure.
  * \paran subpage - not yet implemented
  * \param ix - Record number of page in page.idx (not the address!)
  * \return Might be useful to return something 
  */
 void LinkPage(uint8_t mag, uint8_t page, uint8_t subpage, uint16_t ix)
 {
	NODEPTR np, newnodeptr;
	uint16_t cellAddress;
	DISPLAYNODE node;
	mag=(mag-1) & 0x07; // mags are 0 to 7 in this array
	// What is the address of this page? 
	cellAddress=((mag<<8)+page)*sizeof(NODEPTR);
	xprintf(PSTR("[LinkPage] Enters page ix=%d cell=%d\n\r"),ix,cellAddress);
	np=GetNodePtr(&cellAddress);
	// Is the cell empty?
	
	// HACK ALERT
	// Last page in is the one that is displayed.
	// WARNING: Multiple pages will cause pointers to be leaked.
	// This won't work for sub pages.
	// It needs to be worked on.
	if (np==NULLPTR || true) // This forces the LAST page to be the one that goes to the output
	{
	// TODO: WHAT WE NEED TO DO HERE at the very least, is to reuse the old node if there was one.
		// Yes!
		newnodeptr=NewNode(); // Make a new node
		SetNodePtr(newnodeptr,cellAddress); // Pop it into the PageArray
		node.pageindex=ix;			// Construct the node
		node.subpage=subpage;
		node.next=NULLPTR;
		SetNode(&node,newnodeptr);		
	}
	else
		xprintf(PSTR("[LinkPage] Sorry, carousels are NOT implemented (np=%d)\n\r"),np);
	xprintf(PSTR("[LinkPage] Exits\n\r"));
 } // LinkPage
 
 /** This takes the page.idx list and makes a sorted display list out of it
  * We need to look at all the pages and extract their MPPSS
  * \return 0 if OK, >0 if failed 
  */
 uint8_t ScanPageList(void)
 {
	//FIL PageIndex;		// page.idx. now listFIL
	//FIL PageAll;	// page.all. now pagefileFIL	
	BYTE drive=0;
	FRESULT res;	
	PAGEINDEXRECORD ixRec;
	UINT charcount;	
	PAGE page;
	PAGE *p=&page;
	uint16_t ix;
	const unsigned char MAXLINE=80;
	
	char line[MAXLINE];
	char *str;

	
	// Open the drive and navigate to the correct location
	res=(WORD)disk_initialize(drive);	// di0
	put_rc(f_mount(drive, &Fatfs[drive]));	// fi0
	put_rc(f_chdir("onair"));	
	
	res=f_open(&listFIL,"pages.idx",FA_READ);
	if (res)
	{
		xprintf(PSTR("[displaylist]Epic Fail. Can not open pages.idx\n"));			
		put_rc(res);
		// At this point we might try to create page.all and pages.idx
		return 1;
	}
	
	res=f_open(&pagefileFIL,"pages.all",FA_READ);
	if (res)
	{
		xprintf(PSTR("[displaylist]Epic Fail. Can not open page.all\n"));			
		put_rc(res);
		f_close(&listFIL);
		return 1;
	}

	spiram_init();
	SetSPIRamStatus(SPIRAM_MODE_SEQUENTIAL);

	// For all of the pages in our index...
	for (ix=0;!f_eof(&listFIL);ix++)
	{
		f_read(&listFIL,&ixRec,sizeof(ixRec),&charcount);
		// xprintf(PSTR("seek %ld size %d \n\r"),ixRec.seekptr,ixRec.pagesize);
		// TODO: Use seekptr on page.all and parse the page
		f_lseek(&pagefileFIL,ixRec.seekptr);	// and set the pointer back to the start
		// TODO: Extract the M PP SS fields
		p->mag=9;
		while (p->mag==9) // TODO: Prevent this from going badly wrong!!!
		{
			str=f_gets(line,MAXLINE,&pagefileFIL);		
			// xprintf(PSTR("parsing %s\n\r"),str);
			ParseLine(p,str);
			//TODO: Check that we didn't read past the end of this page
		}
		xprintf(PSTR("M PP SS %1d %02X %02d\n\r"),p->mag,p->page,p->subpage);
		// TODO: Find or create the root of the mag M 
		// Something like 
		LinkPage(p->mag,p->page,p->subpage,ix);
		xprintf(PSTR("next iteration\n\r"));
	}
	f_close(&listFIL);
	f_close(&pagefileFIL);
	xprintf(PSTR("[ScanPageList] Exits\n\r"));
	return 0;
 } // ScanPageList
  
 /** Set up all the lists.
  * Scan all the existing pages and make a sorted list
  */
 uint8_t InitDisplayList(void)
 {
	xprintf(PSTR("[InitDisplayList] Started\n\r"));
	spiram_init();
	SetSPIRamStatus(SPIRAM_MODE_SEQUENTIAL);
	
	sDisplayList=NULLPTR;
	sFreeList=NULLPTR;
	// Put all the slots into the free list
	MakeFreeList();
	Dump();
	// Now scan the pages list and make a sorted list, creating nodes for Root, Node and Junction
	uint8_t result=ScanPageList();
	xprintf(PSTR("[InitDisplayList] Exits\n\r"));
	return result;
 } // initDisplayList
 
 
