#include "findUvOverlaps2.h"
#include "testCase.h"
#include "uvPoint.h"

#include <maya/MArgDatabase.h>
#include <maya/MArgList.h>
#include <maya/MGlobal.h>
#include <maya/MIntArray.h>
#include <maya/MSelectionList.h>
#include <maya/MString.h>
#include <maya/MStringArray.h>
#include <maya/MThreadUtils.h>
#include <maya/MTimer.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

FindUvOverlaps2::FindUvOverlaps2()
{
}

FindUvOverlaps2::~FindUvOverlaps2()
{
}

MSyntax FindUvOverlaps2::newSyntax()
{
    MSyntax syntax;
    syntax.addArg(MSyntax::kString);
    syntax.addFlag("-v", "-verbose", MSyntax::kBoolean);
    syntax.addFlag("-set", "-uvSet", MSyntax::kString);
    return syntax;
}

MStatus FindUvOverlaps2::doIt(const MArgList& args)
{
    MStatus status;

    MSelectionList sel;

    MArgDatabase argData(syntax(), args);

    status = argData.getCommandArgument(0, sel);
    if (status != MS::kSuccess) {
        MGlobal::displayError("You have to provide an object path");
        return MStatus::kFailure;
    }

    sel.getDagPath(0, mDagPath);
    mFnMesh.setObject(mDagPath);

    if (argData.isFlagSet("-verbose"))
        argData.getFlagArgument("-verbose", 0, verbose);
    else
        verbose = false;

    if (argData.isFlagSet("-uvSet"))
        argData.getFlagArgument("-uvSet", 0, uvSet);
    else
        uvSet = mFnMesh.currentUVSetName();

    // Check if specified object is geometry or not
    status = mDagPath.extendToShape();
    CHECK_MSTATUS_AND_RETURN_IT(status);

    // Check if specified uv set exists
    MStringArray uvSetNames;
    bool uvSetFound = false;
    mFnMesh.getUVSetNames(uvSetNames);
    for (unsigned int uv = 0; uv < uvSetNames.length(); uv++) {
        MString& uvSetName = uvSetNames[uv];
        if (uvSetName == uvSet) {
            uvSetFound = true;
            uvSet = uvSetName;
            break;
        }
    }
    if (!uvSetFound) {
        MGlobal::displayError("UV set not found");
        return MS::kFailure;
    }

    if (uvSetFound == false) {
        MGlobal::displayError("Error: uv set not found\n");
        return MS::kFailure;
    }

    if (mDagPath.apiType() != MFn::kMesh) {
        MGlobal::displayError("Selected object is not mesh.");
        return MStatus::kFailure;
    }

    if (verbose)
        MGlobal::displayInfo("Target object : " + mDagPath.fullPathName());
    MGlobal::displayInfo("UVset for check : " + uvSet);

    return redoIt();
}

MStatus FindUvOverlaps2::redoIt()
{
    MStatus status;

    MTimer timer;
    MString timeStr;
    timer.beginTimer();

    const MString* uvSetPtr = &uvSet;
    
    // getUvShellsIds function gives wrong number of uv shells when accessing
    // to non-current uvSets. So just temporarily switch to target uvSet then switch
    // back to current uvSet at the end of function.
    MString currentUvSet = mFnMesh.currentUVSetName();
    if (uvSet != currentUvSet) {
        mFnMesh.setCurrentUVSetName(uvSet);
    }
    
    MIntArray uvShellIds;
    unsigned int nbUvShells;
    mFnMesh.getUvShellsIds(uvShellIds, nbUvShells, uvSetPtr);

    // if no UVs are detected on this mesh
    if (nbUvShells == 0) {
        MGlobal::displayError("No UVs are found.");
        return MS::kFailure;
    }

    int numUVs = mFnMesh.numUVs(uvSet);
    int numPolygons = mFnMesh.numPolygons();

    // Setup uv shell objects
    std::vector<UvShell> uvShellArray;
    uvShellArray.resize(nbUvShells);
    for (unsigned int i = 0; i < nbUvShells; i++) {
        UvShell shell;
        shell.shellIndex = i;
        uvShellArray[i] = shell;
    }

    // Get UV values and add them to the shell
    for (int uvId = 0; uvId < numUVs; uvId++) {
        float u, v;
        mFnMesh.getUV(uvId, u, v, uvSetPtr);
        UvShell& currentShell = uvShellArray[uvShellIds[uvId]];
        currentShell.uVector.push_back(u);
        currentShell.vVector.push_back(v);
    }

    // Setup bounding box information for each shell
    for (unsigned int id = 0; id < nbUvShells; id++) {
        UvShell& shell = uvShellArray[id];
        float uMax = *std::max_element(shell.uVector.begin(), shell.uVector.end());
        float vMax = *std::max_element(shell.vVector.begin(), shell.vVector.end());
        float uMin = *std::min_element(shell.uVector.begin(), shell.uVector.end());
        float vMin = *std::min_element(shell.vVector.begin(), shell.vVector.end());
        shell.uMax = uMax;
        shell.vMax = vMax;
        shell.uMin = uMin;
        shell.vMin = vMin;
    }

    // Loop all polygon faces and create edge objects
    for (int faceId = 0; faceId < numPolygons; faceId++) {
        int numPolygonVertices = mFnMesh.polygonVertexCount(faceId);
        for (int localVtx = 0; localVtx < numPolygonVertices; localVtx++) {
            int curLocalIndex;
            int nextLocalIndex;
            if (localVtx == numPolygonVertices - 1) {
                curLocalIndex = localVtx;
                nextLocalIndex = 0;
            } else {
                curLocalIndex = localVtx;
                nextLocalIndex = localVtx + 1;
            }

            // UV indices by local order
            int uvIdA;
            int uvIdB;

            // Check if current polygon face has mapped UVs, if not break this loop and go to next face
            MStatus statusA = mFnMesh.getPolygonUVid(faceId, curLocalIndex, uvIdA, uvSetPtr);
            MStatus statusB = mFnMesh.getPolygonUVid(faceId, nextLocalIndex, uvIdB, uvSetPtr);
            if (statusA != MS::kSuccess || statusB != MS::kSuccess) {
                if (verbose)
                    MGlobal::displayWarning("Non mapped faces are found");
                break;
            }

            int currentShellIndex = uvShellIds[uvIdA];

            // Create edge index from two point index
            // eg. obj1 (1), p1(0), p2(25) will make edge index of 1025
            std::string uvIdSmallStr;
            std::string uvIdBigStr;

            MString idA;
            MString idB;
            if (uvIdA < uvIdB) {
                idA.set(uvIdA);
                idB.set(uvIdB);
            } else {
                idA.set(uvIdB);
                idB.set(uvIdA);
            }
            // MString edgeIndexStr = idA + idB;
            unsigned int edgeIndex = ("1" + idA + idB).asUnsigned();

            // Get UV values and create edge objects
            float u_current, v_current;
            float u_next, v_next;
            mFnMesh.getPolygonUV(faceId, curLocalIndex, u_current, v_current, uvSetPtr);
            mFnMesh.getPolygonUV(faceId, nextLocalIndex, u_next, v_next, uvSetPtr);
            UvPoint p1(u_current, v_current, uvIdA, currentShellIndex);
            UvPoint p2(u_next, v_next, uvIdB, currentShellIndex);

            UvPoint beginPt;
            UvPoint endPt;

            if (p1 > p2) {
                beginPt = p2;
                endPt = p1;
            } else {
                beginPt = p1;
                endPt = p2;
            }

            // Create edge objects and insert them to shell edge set
            UvEdge edge(beginPt, endPt, edgeIndex);
            uvShellArray[currentShellIndex].edgeSet.insert(edge);
        }
    }

    timer.endTimer();
    double time = timer.elapsedTime();
    if (verbose) {
        timeStr.set(time);
        MGlobal::displayInfo("Initialization done : " + timeStr + " seconds");
    }
    timer.clear();

    timer.beginTimer();
	if (nbUvShells == 1) {
		// if there is only one uv shell, just send it to checker command. 
		// don't need to check uv bounding box overlaps check.
		status = check(uvShellArray[0].edgeSet);
		if (status != MS::kSuccess) {
			MGlobal::displayInfo("Error found in shell");
		}
	}
	else {
		// If there multiple uv shells, do BBox overlap check first, then if they overlaps,
		// make one combined shell and send it to checker command
		
		// Countainer for both overlapped shells and indivisual shells for checker
		std::vector<std::set<UvEdge>> shellArray;

		// Array like [0, 1, 3, 4 ...... nbUvShells]
		std::set<int> shellIndices;
		for (unsigned int i = 0; i < nbUvShells; i++) {
			shellIndices.insert(i);
		}

		// Get combinations of shell indices eg. (0, 1), (0, 2), (1, 2),,,
		std::vector<std::vector<int>> shellCombinations;
		makeCombinations(uvShellArray.size(), shellCombinations);

		for (size_t i = 0; i < shellCombinations.size(); i++) {
			UvShell& shellA = uvShellArray[shellCombinations[i][0]];
			UvShell& shellB = uvShellArray[shellCombinations[i][1]];

			if (isShellOverlapped(shellA, shellB)) {
				// Check boundingbox check for two shells
				// If those two shells are overlapped, combine them into one single shell
				// and add to shellArray
				std::set<UvEdge> combinedEdges;
				combinedEdges.insert(shellA.edgeSet.begin(), shellA.edgeSet.end());
				combinedEdges.insert(shellB.edgeSet.begin(), shellB.edgeSet.end());
				shellArray.push_back(combinedEdges);

				// Remove from shellIndices as these shells don't have to be checked
				// as indivisula shells
				shellIndices.erase(shellA.shellIndex);
				shellIndices.erase(shellB.shellIndex);
			}
		}

		std::set<int>::iterator shIter;
		for (shIter = shellIndices.begin(); shIter != shellIndices.end(); ++shIter) {
			int index = *shIter;
			std::set<UvEdge>& tempSet = uvShellArray[index].edgeSet;
			shellArray.push_back(tempSet);
		}

		// Run checker for shells
		// #ifdef _OPENMP
		// #pragma omp parallel for
		// #endif
		for (size_t s = 0; s < shellArray.size(); s++) {
			status = check(shellArray[s]);
			if (status != MS::kSuccess) {
				MGlobal::displayInfo("Error found in shell");
			}
		}
	}

    timer.endTimer();
    if (verbose) {
        timeStr.set(timer.elapsedTime());
        MGlobal::displayInfo("check completed : " + timeStr + " seconds.");
    }
    timer.clear();
    
    // Switch back to the initial uv set
    mFnMesh.setCurrentUVSetName(currentUvSet);

    // Setup return result
    MStringArray resultStrArray;
    MString index;
    for (std::unordered_set<int>::iterator fsi = resultSet.begin(), end = resultSet.end(); fsi != end; ++fsi) {
        index.set(*fsi);
        MString fullPath = mDagPath.fullPathName();
        MString n = fullPath + ".map[" + index + "]";
        resultStrArray.append(n);
    }
    MPxCommand::setResult(resultStrArray);

    return MS::kSuccess;
}

bool FindUvOverlaps2::isShellOverlapped(UvShell& shellA, UvShell& shellB)
{
    if (shellA.uMax < shellB.uMin)
        return false;

    if (shellA.uMin > shellB.uMax)
        return false;

    if (shellA.vMax < shellB.vMin)
        return false;

    if (shellA.vMin > shellB.vMax)
        return false;

    return true;
}

MStatus FindUvOverlaps2::check(const std::set<UvEdge>& edges)
{
    std::deque<Event> eventQueue;

    int eventIndex = 0;
    for (std::set<UvEdge>::iterator iter = edges.begin(), end = edges.end(); iter != end; ++iter) {
        UvEdge edge = *iter;
        Event ev1("begin", edge.begin, edge, eventIndex);
        eventQueue.push_back(ev1);
        eventIndex += 1;
        Event ev2("end", edge.end, edge, eventIndex);
        eventQueue.push_back(ev2);
        eventIndex += 1;
    }
    std::sort(eventQueue.begin(), eventQueue.end());

    std::vector<UvEdge> statusQueue;
    statusQueue.reserve(edges.size());

    while (true) {
        if (eventQueue.empty()) {
            break;
        }
        Event firstEvent = eventQueue.front();
        // UvEdge edge = firstEvent.edge;
        eventQueue.pop_front();
    
        if (firstEvent.status == "begin") {
            doBegin(firstEvent, eventQueue, statusQueue);
        } else if (firstEvent.status == "end") {
            doEnd(firstEvent, eventQueue, statusQueue);
        } else if (firstEvent.status == "intersect") {
            doCross(firstEvent, eventQueue, statusQueue);
        } else {
            MGlobal::displayError("Unknow exception");
            return MS::kFailure;
        }
    }

    return MS::kSuccess;
}

bool FindUvOverlaps2::doBegin(Event& currentEvent, std::deque<Event>& eventQueue, std::vector<UvEdge>& statusQueue)
{
    UvEdge& edge = currentEvent.edge;
    statusQueue.push_back(edge);

    // if there are no edges to compare
    size_t numStatus = statusQueue.size();
    if (numStatus == 1) {
        return false;
    }

    // Update x values of intersection to the sweepline for all edges
    // in the statusQueue
    for (size_t i = 0; i < numStatus; i++) {
        statusQueue[i].setCrossingPointX(currentEvent.v);
    }
    std::sort(statusQueue.begin(), statusQueue.end(), UvEdgeComparator());

    std::vector<UvEdge>::iterator foundIter = std::find(statusQueue.begin(), statusQueue.end(), edge);
	if (foundIter == statusQueue.end()) {
		// not found
		return false;
	}
    size_t index = std::distance(statusQueue.begin(), foundIter);
    if (index == numStatus) {
        // invalid
    }

    UvEdge& currentEdge = statusQueue[index];

    if (index == 0) {
        // If first item, check the next edge
        UvEdge& nextEdge = statusQueue[index + 1];
        checkEdgesAndCreateEvent(currentEdge, nextEdge, eventQueue);
    } else if (index == numStatus - 1) {
        // if last iten in the statusQueue
        UvEdge& previousEdge = statusQueue[index - 1];
        checkEdgesAndCreateEvent(currentEdge, previousEdge, eventQueue);
    } else {
        UvEdge& nextEdge = statusQueue[index + 1];
        UvEdge& previousEdge = statusQueue[index - 1];
        checkEdgesAndCreateEvent(currentEdge, nextEdge, eventQueue);
        checkEdgesAndCreateEvent(currentEdge, previousEdge, eventQueue);
    }
    return true;
}

bool FindUvOverlaps2::doEnd(Event& currentEvent, std::deque<Event>& eventQueue, std::vector<UvEdge>& statusQueue)
{
    UvEdge& edge = currentEvent.edge;
    std::vector<UvEdge>::iterator iter_for_removal = std::find(statusQueue.begin(), statusQueue.end(), edge);
    if (iter_for_removal == statusQueue.end()) {
        MGlobal::displayInfo("error1");
        // if iter not found
        // return MS::kFailure;
        return false;
    }

    size_t removeIndex = std::distance(statusQueue.begin(), iter_for_removal);
    if (removeIndex == statusQueue.size()) {
        MGlobal::displayInfo("error2");
        // invalid
        return MS::kFailure;
        return false;
    }

    if (statusQueue.size() <= 2) {
        // if num items are less than 2 in the countainer, do nothing
    } else if (removeIndex == 0) {
        // if first item, do nothing

    } else if (removeIndex == statusQueue.size() - 1) {
        // if last item, do nothing
    } else {
        // check previous and next edge intersection as they can be next
        // each other after removing the current edge
        UvEdge& nextEdge = statusQueue[removeIndex + 1];
        UvEdge& previousEdge = statusQueue[removeIndex - 1];
        checkEdgesAndCreateEvent(previousEdge, nextEdge, eventQueue);
    }

    // Remove current edge from the statusQueue
    statusQueue.erase(iter_for_removal);
    return true;
}

bool FindUvOverlaps2::doCross(Event& currentEvent, std::deque<Event>& eventQueue, std::vector<UvEdge>& statusQueue)
{
    if (statusQueue.size() <= 2) {
        return false;
    }

    UvEdge& thisEdge = currentEvent.edge;
    UvEdge& otherEdge = currentEvent.otherEdge;
    std::vector<UvEdge>::iterator thisEdgeIter = std::find(statusQueue.begin(), statusQueue.end(), thisEdge);
    std::vector<UvEdge>::iterator otherEdgeIter = std::find(statusQueue.begin(), statusQueue.end(), otherEdge);
	if (thisEdgeIter == statusQueue.end() || otherEdgeIter == statusQueue.end()) {
		// if not found
		return false;
	}
    size_t thisIndex = std::distance(statusQueue.begin(), thisEdgeIter);
    size_t otherIndex = std::distance(statusQueue.begin(), otherEdgeIter);
    size_t small, big;

    if (thisIndex > otherIndex) {
        small = otherIndex;
        big = thisIndex;
    } else {
        small = thisIndex;
        big = otherIndex;
    }

    if (small == 0) {
        UvEdge& firstEdge = statusQueue[small];
        UvEdge& secondEdge = statusQueue[big + 1];
        checkEdgesAndCreateEvent(firstEdge, secondEdge, eventQueue);
    } else if (big == statusQueue.size() - 1) {
        UvEdge& firstEdge = statusQueue[small - 1];
        UvEdge& secondEdge = statusQueue[big];
        checkEdgesAndCreateEvent(firstEdge, secondEdge, eventQueue);
    } else {
        UvEdge& firstEdge = statusQueue[small - 1];
        UvEdge& secondEdge = statusQueue[small];
        UvEdge& thirdEdge = statusQueue[big];
        UvEdge& forthEdge = statusQueue[big + 1];

        checkEdgesAndCreateEvent(firstEdge, thirdEdge, eventQueue);
        checkEdgesAndCreateEvent(secondEdge, forthEdge, eventQueue);
    }
    return false;
}

MStatus FindUvOverlaps2::checkEdgesAndCreateEvent(UvEdge& edgeA, UvEdge& edgeB, std::deque<Event>& eventQueue)
{
    bool isParallel = false;
    if (edgeA.isIntersected(edgeB, isParallel, intersect_u, intersect_v)) {
        int ids[] = { edgeA.beginIndex, edgeA.endIndex, edgeB.beginIndex, edgeB.endIndex };
        resultSet.insert(ids, ids + 4);
        if (isParallel == false) {
            Event crossEvent("intersect", intersect_u, intersect_v, edgeA, edgeB);
            eventQueue.push_back(crossEvent);
            std::sort(eventQueue.begin(), eventQueue.end());
        }
    }
    return MS::kSuccess;
}

/* https://stackoverflow.com/questions/12991758/creating-all-possible-k-combinations-of-n-items-in-c */
void FindUvOverlaps2::makeCombinations(size_t N, std::vector<std::vector<int>>& vec)
{
    std::string bitmask(2, 1); // K leading 1's
    bitmask.resize(N, 0); // N-K trailing 0's

    // print integers and permute bitmask
    do {
        std::vector<int> sb;
        for (size_t i = 0; i < N; ++i) {
            if (bitmask[i]) {
                sb.push_back(i);
            }
        }
        vec.push_back(sb);
    } while (std::prev_permutation(bitmask.begin(), bitmask.end()));
}

MStatus FindUvOverlaps2::undoIt()
{
    return MS::kSuccess;
}

bool FindUvOverlaps2::isUndoable() const
{
    return false;
}

void* FindUvOverlaps2::creator()
{
    return new FindUvOverlaps2;
}
