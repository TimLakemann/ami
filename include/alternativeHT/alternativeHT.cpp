#include "alternativeHT.h"

#include <bits/stdc++.h>

using namespace uvdar;

alternativeHT::alternativeHT(double decayFactor, int polyOrder){

    extended_search_ = std::make_unique<ExtendedSearch>(decayFactor, polyOrder);

}

void alternativeHT::setDebugFlags(bool debug, bool visual_debug){
    debug_ = debug;
    visual_debug_ = visual_debug;
}

void alternativeHT::setSequences(std::vector<std::vector<bool>> i_sequences){
  
    originalSequences_ = i_sequences;
    matcher_ = std::make_unique<SignalMatcher>(originalSequences_);
}

void alternativeHT::updateFramerate(double input) {
  if (input > 1.0)
    framerate_ = input;
}

void alternativeHT::processBuffer(const mrs_msgs::ImagePointsWithFloatStampedConstPtr ptsMsg) {
    
    std::vector<PointState> currentFrame;
    for ( auto pointWithTimeStamp : ptsMsg->points) {
        PointState p;
        p.point = cv::Point(pointWithTimeStamp.x, pointWithTimeStamp.y);
        p.ledState = true;
        p.insertTime = ptsMsg->stamp;
        currentFrame.push_back(p);
    }

    // std::cout << "Sequences:\n";
    // for( auto k : generatedSequences_){
    //     for(auto r : k){
    //         if(r.ledState) std::cout << "1,"; 
    //         else std::cout << "0,";
    //     }
    //     std::cout << "\n";
    // }
    findClosestPixelAndInsert(currentFrame);

    cleanPotentialBuffer();  // TODO: NOT WORKING!!!!!
  
}

void alternativeHT::findClosestPixelAndInsert(std::vector<PointState> & currentFrame) {   
    
    // reference to sequences used for processing in the moving average functions
    std::vector<seqPointer> pGenSequence;
    {
       std::scoped_lock lock(mutex_generatedSequences_);
        for(auto & seq : generatedSequences_){
            pGenSequence.push_back(&seq);
        }
    }

    std::vector<PointState> noNN;
    for(auto & currPoint : currentFrame){
        std::scoped_lock lock(mutex_generatedSequences_);
        bool nearestNeighbor = false;
        for(auto seq = pGenSequence.begin(); seq != pGenSequence.end(); ++seq){
            PointState lastInserted = (*seq)->end()[-1];
            cv::Point2d diff = computeXYDiff(currPoint.point, lastInserted.point);
            if(diff.x <= max_pixel_shift_x_ && diff.y <= max_pixel_shift_y_){
                nearestNeighbor = true;
                insertPointToSequence(*(*seq), currPoint);    
                pGenSequence.erase(seq);
                break;
            }else{
                nearestNeighbor = false;
            }
        }
        if(nearestNeighbor == false){
            noNN.push_back(currPoint);
        }
    }
    expandedSearch(noNN, pGenSequence);

}

cv::Point2d alternativeHT::computeXYDiff(const cv::Point2d first, const cv::Point2d second){
    
    cv::Point2d difference; 
    difference.x = std::abs(first.x - second.x);
    difference.y = std::abs(first.y - second.y);
    return difference;
}

void alternativeHT::insertPointToSequence(std::vector<PointState> & sequence, const PointState signal){
        sequence.push_back(signal);            
        if(sequence.size() > (originalSequences_[0].size()*size_for_savedSequences_)){
            sequence.erase(sequence.begin());
        }
}


void alternativeHT::expandedSearch(std::vector<PointState> & noNNCurrentFrame, std::vector<seqPointer> sequencesNoInsert){
    
    std::scoped_lock lock(mutex_generatedSequences_);
    
    if(noNNCurrentFrame.size() != 0){
        double insertTime = noNNCurrentFrame[0].insertTime.toSec() + predictionMargin_;

        // std::vector<SeqWithTrajectory> sequences;
        for(int k = 0; k < sequencesNoInsert.size(); ++k){
            
            if(!checkSequenceValidityWithNewInsert(sequencesNoInsert[k])){
                continue;
            }
            SeqWithTrajectory seqTrajectory;
            seqTrajectory.seq = sequencesNoInsert[k];
            
            if(!extended_search_->selectPointsForRegressionAndDoRegression(seqTrajectory)){
                continue;
            }

            calculatePredictionTriangle(seqTrajectory, insertTime);
    

            bool coffAllZero = false;
            if(!checkCoeffValidity(seqTrajectory)){
                coffAllZero = true;
            }
            cv::Point2d firstPoint   =  seqTrajectory.seq->end()[-1].firstEdgeTri;
            cv::Point2d secondPoint  =  seqTrajectory.seq->end()[-1].secEdgeTri;
            cv::Point2d initialPoint =  seqTrajectory.seq->end()[-1].point;
            for(int i = 0; i < noNNCurrentFrame.size(); ++i){
                if(!coffAllZero){
                    if(extended_search_->isInside(firstPoint, secondPoint, initialPoint, noNNCurrentFrame[i].point)){
                        insertPointToSequence(*(sequencesNoInsert[k]), noNNCurrentFrame[i]);
                        noNNCurrentFrame.erase(noNNCurrentFrame.begin()+i);
                        sequencesNoInsert.erase(sequencesNoInsert.begin()+k); 
                        break;
                    }
                }
                cv::Point2d diff = computeXYDiff(sequencesNoInsert[k]->end()[-1].point, noNNCurrentFrame[i].point);
                if(diff.x <= max_pixel_shift_x_+4 && diff.y <= max_pixel_shift_y_+4){
                    insertPointToSequence(*(sequencesNoInsert[k]), noNNCurrentFrame[i]);
                    noNNCurrentFrame.erase(noNNCurrentFrame.begin()+i);
                    sequencesNoInsert.erase(sequencesNoInsert.begin()+k);
                    std::cout << "hit box\n";

                    break;
                }
            }
        }
    }


    // insert VP to sequnces were no point was inserted
    for(auto seq : sequencesNoInsert){
        insertVPforSequencesWithNoInsert(seq);
    }

    // if still points are not inserted start new sequence
    for(auto point : noNNCurrentFrame){
        std::vector<PointState> vect;
        vect.push_back(point);
        generatedSequences_.push_back(vect);
    }
}

bool alternativeHT::checkSequenceValidityWithNewInsert(const seqPointer & seq){

    if(seq->size() > 1){
        // if the last two led states were on - no "on" point expected -> no NN search necessary
       if(seq->end()[-1].ledState == true && seq->end()[-2].ledState == true){
            return false;
        }
    }
    return true; 
}

// if all coefficients are zero return false
bool alternativeHT::checkCoeffValidity(const SeqWithTrajectory & trajectory){


    int xCount = 0, yCount = 0;
    for(auto coff : trajectory.xCoeff){
        if(coff == 0.0 ){
            xCount++;
        }
    }
    for(auto coff : trajectory.yCoeff){
        if(coff == 0.0 ){
            yCount++;
        }
    }
    // if all coefficients are zero 
    if(yCount == (int)trajectory.yCoeff.size() && xCount == (int)trajectory.xCoeff.size()){
        return false;
    }
    return true; 

}

void alternativeHT::calculatePredictionTriangle(SeqWithTrajectory & path, const double predictionTime){

    double xPredict = 0; 
    double yPredict = 0;

    for(int i = 0; i < (int)path.xCoeff.size(); ++i){
        xPredict += path.xCoeff[i]*pow(predictionTime, i); 
    }
    for(int i = 0; i < (int)path.yCoeff.size(); ++i){
        yPredict += path.yCoeff[i]*pow(predictionTime, i);
    }
    cv::Point2d predictedPoint = cv::Point2d(xPredict, yPredict);

    cv::Point2d groundPointTriangle =  path.seq->end()[-1].point;

    // for( auto it = path.seq->end(); it != path.seq->begin(); --it ){
    //     if(it->point.x != path.seq->end()[-1].point.x || it->point.y != path.seq->end()[-1].point.y){
    //         groundPointTriangle = it->point;
    //         break;
    //     }
    // }
    predictedPoint.x = std::round(predictedPoint.x); 
    predictedPoint.y = std::round(predictedPoint.y);  
    cv::Point2d diffVect = predictedPoint - groundPointTriangle;

    double len =  ( sqrt(pow(diffVect.x,2) + pow(diffVect.y, 2)) );

    if(len < 2){
        diffVect = diffVect*4;
    }else if(len < 6){
        diffVect = diffVect*2; 
    }

    len = ( sqrt(pow(diffVect.x,2) + pow(diffVect.y, 2)) );

    if(len > 20){
        len = len *2/3;
    }

    std::vector<cv::Point2d> orthoVects = extended_search_->findOrthogonalVectorWithLength(diffVect, len);
    // construct triangle in coordinate center
    cv::Point2d firstEdgeCenter   = diffVect + orthoVects[0];
    cv::Point2d secEdgeCenter   = diffVect + orthoVects[1]; 

    //project to the last seen point
    cv::Point2d firstEdge = groundPointTriangle + firstEdgeCenter;
    cv::Point2d secEdge = groundPointTriangle + secEdgeCenter; 

    firstEdge.x = firstEdge.x;
    firstEdge.y = firstEdge.y;
    secEdge.x = secEdge.x;
    secEdge.y = secEdge.y;

    
    path.seq->end()[-1].lengthToPredict = len;
    path.seq->end()[-1].firstEdgeTri    = firstEdge;
    path.seq->end()[-1].secEdgeTri      = secEdge;
    path.seq->end()[-1].predictedNextPoint = predictedPoint;
}

void alternativeHT::insertVPforSequencesWithNoInsert(seqPointer & seq){
    
    PointState pVirtual;
    pVirtual.insertTime = ros::Time::now();
    // select eventually predicted point and not last inserted one 
    pVirtual.point = seq->end()[-1].point;
    pVirtual.ledState = false;
    insertPointToSequence(*seq, pVirtual);
}

void alternativeHT::cleanPotentialBuffer(){

    std::scoped_lock lock(mutex_generatedSequences_);
    double timeMargin = 5/60;
    if(framerate_ != 0){
        timeMargin = (1/framerate_) * 60; 
    }
    // std::cout << "Time margin " << timeMargin; 

    for (auto it = generatedSequences_.begin(); it != generatedSequences_.end(); ++it){ 
        if (it->empty()){
            continue;
        }

        // if( it->size() >= 2){
        //     if(!it->end()[-1].ledState && !it->end()[-2].ledState && !it->end()[-3].ledState){
        //         it = generatedSequences_.erase(it);
        //         std::cout << "delete here\n"; 
        //         continue;
        //     }
        // }
        // double insertTime = it->end()[-1].insertTime.toSec(); 
        // double timeDiffLastInsert = std::abs(insertTime - ros::Time::now().toSec());
        // if (timeDiffLastInsert > timeMargin){ //&& i != generatedSequences_.end()){
        //     ROS_ERROR("TIME");
        //     it = generatedSequences_.erase(it);
        //     continue;
        // }

    }
}

std::vector<std::pair<PointState, int>> alternativeHT::getResults(){

    std::scoped_lock lock(mutex_generatedSequences_);
    std::vector<std::pair<PointState, int>> retrievedSignals;

    for (auto sequence : generatedSequences_){
        std::vector<bool> ledStates;

        std::vector<PointState> selected;
        if ((int)sequence.size() > (int)originalSequences_[0].size()){
            int diff = (int)sequence.size() - (int)originalSequences_[0].size();
            // std::cout << "the diff" << diff << "\n";

            for(int i = diff; i < sequence.size(); ++i){
                selected.push_back(sequence[i]);
            }
        }else{
            selected = sequence;
        }
        // std::cout << "size " << selected.size() << "\n";
        for (auto point : selected){
            ledStates.push_back(point.ledState);
        }
        // std::cout << "The seq" << std::endl; 
        // for(auto k : ledStates){
        //     if(k)std::cout << "1,";
        //     else std::cout << "0,";
        // }
        // std::cout << std::endl;


        int id = findSequenceMatch(ledStates);
        retrievedSignals.push_back(std::make_pair(sequence.end()[-1], id));
    }

    return retrievedSignals;
}

int alternativeHT::findSequenceMatch(std::vector<bool> sequence){

    for ( int i = 2; i < (int)sequence.size(); i++) {
        if (sequence[i] == false && sequence[i-1] == false && sequence[i-2] == false){
            return -2;
        }
    }
    int id = matcher_->matchSignalWithCrossCorr(sequence);

    return id;
}

alternativeHT::~alternativeHT() {
    // delete generatedSequences_;
}